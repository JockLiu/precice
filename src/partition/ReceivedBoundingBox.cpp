#include "partition/ReceivedBoundingBox.hpp"
#include <map>
#include <vector>
#include "com/CommunicateBoundingBox.hpp"
#include "com/Communication.hpp"
#include "m2n/M2N.hpp"
#include "mapping/Mapping.hpp"
#include "mesh/Mesh.hpp"
#include "utils/Helpers.hpp"
#include "utils/MasterSlave.hpp"


namespace precice
{
namespace partition
{

ReceivedBoundingBox::ReceivedBoundingBox(
    mesh::PtrMesh mesh, double safetyFactor)
    : Partition(mesh),
      _bb(mesh->getDimensions(), std::make_pair(std::numeric_limits<double>::max(), std::numeric_limits<double>::lowest())),
      _dimensions(mesh->getDimensions()),
      _safetyFactor(safetyFactor)
{
}

void ReceivedBoundingBox::communicateBoundingBox()
{
  TRACE();

  if (not utils::MasterSlave::_slaveMode) {
    _m2n->getMasterCommunication()->receive(_remoteParComSize, 0);

    // construct and initialize _remoteBBM
    mesh::Mesh::BoundingBox initialBB;
    for (int i = 0; i < _dimensions; i++) {
      initialBB.push_back(std::make_pair(-1, -1));
    }
    for (int remoteRank = 0; remoteRank < _remoteParComSize; remoteRank++) {
      _remoteBBM[remoteRank] = initialBB;
    }

    // master receives global_bb from other master
    com::CommunicateBoundingBox(_m2n->getMasterCommunication()).receiveBoundingBoxMap(_remoteBBM, 0);
  }
}

void ReceivedBoundingBox::computeBoundingBox()
{
  TRACE();

  /// @todo handle coupling mode (i.e. serial participant)

  prepareBoundingBox();

  if (utils::MasterSlave::_masterMode) { // Master
    assertion(utils::MasterSlave::_rank == 0);
    assertion(utils::MasterSlave::_size > 1);

    // broadcast _remoteBBM to all slaves
    utils::MasterSlave::_communication->broadcast(_remoteParComSize);
    com::CommunicateBoundingBox(utils::MasterSlave::_communication).broadcastSendBoundingBoxMap(_remoteBBM);

    // map from local rank to connected remote ranks
    std::map<int, std::vector<int>> feedbackMap;

    // initialize _feedbackmap
    for (int rank_slave = 1; rank_slave < utils::MasterSlave::_size; rank_slave++) {
      feedbackMap[rank_slave].push_back(-1);
    }

    // feedback for master rank
    std::vector<int> feedback;
    for (auto &remoteBB : _remoteBBM) {
      if (overlapping(_bb, remoteBB.second)) {
        feedback.push_back(remoteBB.first);
      }
    }
    if (feedback.size() != 0)
      feedbackMap[0] = feedback;

    // receive feedback from slaves and add them to feedbackMap
    for (int rank = 1; rank < utils::MasterSlave::_size; rank++) {
      int feedbackSize = 0;
      utils::MasterSlave::_communication->receive(feedbackSize, rank);
      if (feedbackSize != 0) {
        std::vector<int> slaveFeedback;
        utils::MasterSlave::_communication->receive(slaveFeedback, rank);
        feedbackMap[rank] = slaveFeedback;
      }
    }

    // send feedbackMap to other master
    _m2n->getMasterCommunication()->send((int) feedbackMap.size(), 0);
    if (feedbackMap.size() != 0) { // @todo we need an error message here instead
      com::CommunicateBoundingBox(_m2n->getMasterCommunication()).sendFeedbackMap(feedbackMap, 0);
    }

  } else if (utils::MasterSlave::_slaveMode) {
    utils::MasterSlave::_communication->broadcast(_remoteParComSize, 0);

    // construct and initialize _remoteBBM
    mesh::Mesh::BoundingBox initialBB;
    for (int i = 0; i < _dimensions; i++) {
      initialBB.push_back(std::make_pair(-1, -1));
    }
    for (int remoteRank = 0; remoteRank < _remoteParComSize; remoteRank++) {
      _remoteBBM[remoteRank] = initialBB;
    }

    // receive _remoteBBM from master
    com::CommunicateBoundingBox(utils::MasterSlave::_communication).broadcastReceiveBoundingBoxMap(_remoteBBM);

    std::vector<int> feedback;
    for (auto &remoteBB : _remoteBBM) {
      if (overlapping(_bb, remoteBB.second)) {
        feedback.push_back(remoteBB.first);
      }
    }

    // send feedback size to master
    utils::MasterSlave::_communication->send((int) feedback.size(), 0);

    // to prevent sending empty vector!
    if (feedback.size() != 0)
      utils::MasterSlave::_communication->send(feedback, 0);
  }
}

bool ReceivedBoundingBox::overlapping(mesh::Mesh::BoundingBox currentBB, mesh::Mesh::BoundingBox receivedBB)
{
  /*
   * Here two bounding boxes are compared to check whether they overlap or not!
   * Comparison is done for all dimensions and, of course, to have a proper overlap,
   * each dimension must overlap.
   * We need to check if first AND second is smaller than first of the other BB to prevent false negatives
   * due to empty bounding boxes.
   */
  for (int i = 0; i < _dimensions; i++) {
    if ((currentBB[i].first < receivedBB[i].first && currentBB[i].second < receivedBB[i].first) || (receivedBB[i].first < currentBB[i].first && receivedBB[i].second < currentBB[i].first)) {
      return false;
    }
  }
  return true;
}

void ReceivedBoundingBox::prepareBoundingBox()
{
  TRACE(_safetyFactor);

  _bb.resize(_dimensions, std::make_pair(std::numeric_limits<double>::max(), std::numeric_limits<double>::lowest()));

  //create BB around both "other" meshes
  if (_fromMapping.use_count() > 0) {
    auto other_bb = _fromMapping->getOutputMesh()->getBoundingBox();
    for (int d = 0; d < _dimensions; d++) {
      if (_bb[d].first > other_bb[d].first)
        _bb[d].first = other_bb[d].first;
      if (_bb[d].second < other_bb[d].second)
        _bb[d].second = other_bb[d].second;
    }
  }
  if (_toMapping.use_count() > 0) {
    auto other_bb = _toMapping->getInputMesh()->getBoundingBox();
    for (int d = 0; d < _dimensions; d++) {
      if (_bb[d].first > other_bb[d].first)
        _bb[d].first = other_bb[d].first;
      if (_bb[d].second < other_bb[d].second)
        _bb[d].second = other_bb[d].second;
    }
  }

  //enlarge BB
  assertion(_safetyFactor >= 0.0);

  double maxSideLength = 1e-6; // we need some minimum > 0 here

  for (int d = 0; d < _dimensions; d++) {
    maxSideLength = std::max(maxSideLength, _bb[d].second - _bb[d].first);
  }
  for (int d = 0; d < _dimensions; d++) {
    _bb[d].second += _safetyFactor * maxSideLength;
    _bb[d].first -= _safetyFactor * maxSideLength;
    DEBUG("Merged BoundingBox, dim: " << d << ", first: " << _bb[d].first << ", second: " << _bb[d].second);
  }
}

void ReceivedBoundingBox::communicate()
{
}
void ReceivedBoundingBox::compute()
{
}
void ReceivedBoundingBox::createOwnerInformation()
{
}

} // namespace partition
} // namespace precice
