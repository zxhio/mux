//===- errors.cpp - Custom error --------------------------------*- C++ -*-===//
//
/// \file
/// Custom error implement.
//
// Author:  zxh
// Date:    2024/04/17 14:27:09
//===----------------------------------------------------------------------===//

#include "errors.h"

const std::error_category &address_category() {
  static AddressCategory c;
  return c;
}