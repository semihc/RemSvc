#ifndef ASSERT_HH
#define ASSERT_HH
#pragma once

#include <source_location>


namespace RS {
    
  void Expects(bool v, const std::source_location loc = std::source_location::current());
  void Ensures(bool v, const std::source_location loc = std::source_location::current());
    
};


     
#endif /* Include guard */
