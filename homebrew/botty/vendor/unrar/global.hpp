#ifndef _RAR_GLOBAL_
#define _RAR_GLOBAL_

#ifdef INCLUDEGLOBAL
  #define EXTVAR
#else
  #define EXTVAR extern
#endif

#ifdef RARDLL
// Botty opens independent archives on two workers. Error state must not leak
// between handles on different threads (notably cancellation and CRC failures).
ErrorHandler& BottyRarErrorHandler();
#define ErrHandler BottyRarErrorHandler()
#else
EXTVAR ErrorHandler ErrHandler;
#endif



#endif
