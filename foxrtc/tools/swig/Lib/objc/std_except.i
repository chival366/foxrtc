/* -----------------------------------------------------------------------------
 * std_except.i
 *
 * Typemaps used by the STL wrappers that throw exceptions.
 * These typemaps are used when methods are declared with an STL exception specification, such as
 *   size_t at() const throw (std::out_of_range);
 * ----------------------------------------------------------------------------- */

%{
#include <stdexcept>
%}

namespace std 
{
  %ignore exception;
  struct exception {};
}

%typemap(throws) std::bad_exception     "SWIG_ObjcThrowException(SWIG_ObjcRuntimeException, $1.what());\n return $null;"
%typemap(throws) std::domain_error      "SWIG_ObjcThrowException(SWIG_ObjcRuntimeException, $1.what());\n return $null;"
%typemap(throws) std::exception         "SWIG_ObjcThrowException(SWIG_ObjcRuntimeException, $1.what());\n return $null;"
%typemap(throws) std::invalid_argument  "SWIG_ObjcThrowException(SWIG_ObjcIllegalArgumentException, $1.what());\n return $null;"
%typemap(throws) std::length_error      "SWIG_ObjcThrowException(SWIG_ObjcIndexOutOfBoundsException, $1.what());\n return $null;"
%typemap(throws) std::logic_error       "SWIG_ObjcThrowException(SWIG_ObjcRuntimeException, $1.what());\n return $null;"
%typemap(throws) std::out_of_range      "SWIG_ObjcThrowException(SWIG_ObjcIndexOutOfBoundsException, $1.what());\n return $null;"
%typemap(throws) std::overflow_error    "SWIG_ObjcThrowException(SWIG_ObjcArithmeticException, $1.what());\n return $null;"
%typemap(throws) std::range_error       "SWIG_ObjcThrowException(SWIG_ObjcIndexOutOfBoundsException, $1.what());\n return $null;"
%typemap(throws) std::runtime_error     "SWIG_ObjcThrowException(SWIG_ObjcRuntimeException, $1.what());\n return $null;"
%typemap(throws) std::underflow_error   "SWIG_ObjcThrowException(SWIG_ObjcArithmeticException, $1.what());\n return $null;"

