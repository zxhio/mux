//===- errors.h - Custom error ----------------------------------*- C++ -*-===//
//
/// \file
/// Custom error code and category defines.
//
// Author:  zxh
// Date:    2024/04/17 14:25:13
//===----------------------------------------------------------------------===//

#pragma

#include <system_error>

enum AddressErrors {
  AddrErrNone,
  AddrErrInvalidPort,
  AddrErrMissingPort,
  AddrErrTooManyColons,
  AddrErrMissingOpenBrackets,
  AddrErrMissingClosedBrackets,
  AddrErrUnexpectedOpenBrackets,
  AddrPortErrUnexpectedClosedBrackets,
};

class AddressCategory : public std::error_category {
public:
  const char *name() const noexcept override {
    return "SplitHostPortErrorCategory";
  }

  std::string message(int err_code) const override {
    switch (err_code) {
    case AddrErrNone:
      return "success";
    case AddrErrInvalidPort:
      return "invalid port";
    case AddrErrMissingPort:
      return "missing port in address";
    case AddrErrTooManyColons:
      return "too many colons in address";
    case AddrErrMissingOpenBrackets:
      return "missing '[' in address";
    case AddrErrMissingClosedBrackets:
      return "missing ']' in address";
    case AddrErrUnexpectedOpenBrackets:
      return "unexpected '[' in address";
    case AddrPortErrUnexpectedClosedBrackets:
      return "unexpected ']' in address";
    default:
      return "unknown error";
    }
  }
};

const std::error_category &address_category();