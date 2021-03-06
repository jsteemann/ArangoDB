////////////////////////////////////////////////////////////////////////////////
/// @brief ssl helper functions
///
/// @file
///
/// DISCLAIMER
///
/// Copyright 2014 ArangoDB GmbH, Cologne, Germany
/// Copyright 2004-2014 triAGENS GmbH, Cologne, Germany
///
/// Licensed under the Apache License, Version 2.0 (the "License");
/// you may not use this file except in compliance with the License.
/// You may obtain a copy of the License at
///
///     http://www.apache.org/licenses/LICENSE-2.0
///
/// Unless required by applicable law or agreed to in writing, software
/// distributed under the License is distributed on an "AS IS" BASIS,
/// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
/// See the License for the specific language governing permissions and
/// limitations under the License.
///
/// Copyright holder is ArangoDB GmbH, Cologne, Germany
///
/// @author Jan Steemann
/// @author Copyright 2014, ArangoDB GmbH, Cologne, Germany
/// @author Copyright 2012-2013, triAGENS GmbH, Cologne, Germany
////////////////////////////////////////////////////////////////////////////////

#ifndef ARANGODB_BASICS_SSL__HELPER_H
#define ARANGODB_BASICS_SSL__HELPER_H 1

#include "Basics/Common.h"

#include <openssl/ssl.h>

namespace triagens {
  namespace basics {

// -----------------------------------------------------------------------------
// --SECTION--                                                      public types
// -----------------------------------------------------------------------------

////////////////////////////////////////////////////////////////////////////////
/// @brief SSL protocol methods
////////////////////////////////////////////////////////////////////////////////

        enum protocol_e {
          SSL_UNKNOWN = 0,
          SSL_V2      = 1,
          SSL_V23     = 2,
          SSL_V3      = 3,
          TLS_V1      = 4,

          SSL_LAST
        };

////////////////////////////////////////////////////////////////////////////////
/// @brief SSL_CONST
////////////////////////////////////////////////////////////////////////////////

#if (OPENSSL_VERSION_NUMBER < 0x00999999L)
#define SSL_CONST /* */
#else
#define SSL_CONST const
#endif

// -----------------------------------------------------------------------------
// --SECTION--                                                  public functions
// -----------------------------------------------------------------------------

////////////////////////////////////////////////////////////////////////////////
/// @brief creates an SSL context
////////////////////////////////////////////////////////////////////////////////

    SSL_CTX* sslContext (protocol_e, std::string const& keyfile);

////////////////////////////////////////////////////////////////////////////////
/// @brief get the name of an SSL protocol version
////////////////////////////////////////////////////////////////////////////////

    std::string protocolName (const protocol_e protocol);

////////////////////////////////////////////////////////////////////////////////
/// @brief get last SSL error
////////////////////////////////////////////////////////////////////////////////

    std::string lastSSLError ();
  }
}

#endif

// -----------------------------------------------------------------------------
// --SECTION--                                                       END-OF-FILE
// -----------------------------------------------------------------------------

// Local Variables:
// mode: outline-minor
// outline-regexp: "/// @brief\\|/// {@inheritDoc}\\|/// @page\\|// --SECTION--\\|/// @\\}"
// End:
