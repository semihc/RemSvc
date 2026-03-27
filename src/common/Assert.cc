/** \file Assert.cc
 * Definitions for Debugging support.
 *
 *
 *
 */

#include <exception>
#include <stdexcept>
#include <sstream>
#include "Assert.hh"


using namespace std;

namespace RS {

void Expects(bool v, const source_location loc)
{
  //-Expects(v);
  if(!v) {
    ostringstream os;
    os << "Expect failure at " << loc.file_name() << '(' << loc.line() << ':' << loc.column() << ')' << loc.function_name();
    throw runtime_error(os.str());
  }
}

void Ensures(bool v, const source_location loc)
{
  //-Ensures(v);
  if(!v) {
    ostringstream os;
    os << "Ensure failure at " << loc.file_name() << '(' << loc.line() << ':' << loc.column() << ')' << loc.function_name();
    throw runtime_error(os.str());
  }

}


};  // namespace


