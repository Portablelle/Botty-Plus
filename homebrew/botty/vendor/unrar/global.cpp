#define INCLUDEGLOBAL

#ifdef _MSC_VER
#pragma hdrstop
#endif

#include "rar.hpp"

#ifdef RARDLL
#include <pthread.h>
#include <stdexcept>
namespace {
pthread_key_t errorKey;
pthread_once_t errorOnce=PTHREAD_ONCE_INIT;
int errorKeyStatus=0;
void destroyError(void* value){delete static_cast<ErrorHandler*>(value);}
void createErrorKey(){errorKeyStatus=pthread_key_create(&errorKey,destroyError);}
}
ErrorHandler& BottyRarErrorHandler(){
  if(pthread_once(&errorOnce,createErrorKey)||errorKeyStatus)throw std::runtime_error("RAR thread state unavailable");
  auto* value=static_cast<ErrorHandler*>(pthread_getspecific(errorKey));
  if(!value){value=new ErrorHandler;if(pthread_setspecific(errorKey,value)){delete value;throw std::runtime_error("RAR thread state unavailable");}}
  return *value;
}
#endif
