#include "Request.hpp"
#include <memory>

namespace precice {
namespace com {

void Request::wait(std::vector<PtrRequest> &requests)
{
  for (auto request : requests) {
    request->wait();
  }
}

Request::~Request() = default;
} // namespace com
} // namespace precice
