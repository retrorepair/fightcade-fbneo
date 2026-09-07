// Groovy MiSTer - module-internal header.
//
// Shares the one GroovyMister client instance between the files that make up the module.
//
// Never include this from outside src/burner/win32/groovy/. It pulls in groovymister.h and
// therefore <winsock2.h>, while main.cpp includes the Winsock 1.1 <winsock.h>, and MSVC will not
// tolerate both in one translation unit. FBNeo-facing code uses groovy_output.h and
// groovy_input.h, which are socket-free by design.

#ifndef GROOVY_INTERNAL_H
#define GROOVY_INTERNAL_H

#include "groovymister.h"

// Defined in groovy_output.cpp, which owns the session lifecycle.
extern GroovyMister gm;

// True when a session is up and blits are going out. Input polling gates on this.
bool GroovyInternalIsConnected();

#endif // GROOVY_INTERNAL_H
