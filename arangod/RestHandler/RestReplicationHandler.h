////////////////////////////////////////////////////////////////////////////////
/// @brief replication request handler
///
/// @file
///
/// DISCLAIMER
///
/// Copyright 2004-2013 triAGENS GmbH, Cologne, Germany
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
/// Copyright holder is triAGENS GmbH, Cologne, Germany
///
/// @author Jan Steemann
/// @author Copyright 2010-2013, triAGENS GmbH, Cologne, Germany
////////////////////////////////////////////////////////////////////////////////

#ifndef TRIAGENS_REST_HANDLER_REST_REPLICATION_HANDLER_H
#define TRIAGENS_REST_HANDLER_REST_REPLICATION_HANDLER_H 1

#include "RestHandler/RestVocbaseBaseHandler.h"
#include "HttpServer/HttpServer.h"

using namespace triagens::basics;
using namespace triagens::rest;
using namespace std;

struct TRI_replication_state_s;
struct TRI_vocbase_col_s;

namespace triagens {
  namespace arango {

    class RestReplicationHandler : public RestVocbaseBaseHandler {

// -----------------------------------------------------------------------------
// --SECTION--                                            RestReplicationHandler
// -----------------------------------------------------------------------------

// -----------------------------------------------------------------------------
// --SECTION--                                      constructors and destructors
// -----------------------------------------------------------------------------

////////////////////////////////////////////////////////////////////////////////
/// @addtogroup ArangoDB
/// @{
////////////////////////////////////////////////////////////////////////////////

      public:

////////////////////////////////////////////////////////////////////////////////
/// @brief constructor
////////////////////////////////////////////////////////////////////////////////

        RestReplicationHandler (rest::HttpRequest*, struct TRI_vocbase_s*);

////////////////////////////////////////////////////////////////////////////////
/// @brief destructor
////////////////////////////////////////////////////////////////////////////////

        ~RestReplicationHandler ();

////////////////////////////////////////////////////////////////////////////////
/// @}
////////////////////////////////////////////////////////////////////////////////

// -----------------------------------------------------------------------------
// --SECTION--                                                   Handler methods
// -----------------------------------------------------------------------------

////////////////////////////////////////////////////////////////////////////////
/// @addtogroup ArangoDB
/// @{
////////////////////////////////////////////////////////////////////////////////

      public:

////////////////////////////////////////////////////////////////////////////////
/// {@inheritDoc}
////////////////////////////////////////////////////////////////////////////////

        bool isDirect ();

////////////////////////////////////////////////////////////////////////////////
/// {@inheritDoc}
////////////////////////////////////////////////////////////////////////////////

        string const& queue () const;

////////////////////////////////////////////////////////////////////////////////
/// {@inheritDoc}
////////////////////////////////////////////////////////////////////////////////

        Handler::status_e execute();

////////////////////////////////////////////////////////////////////////////////
/// @}
////////////////////////////////////////////////////////////////////////////////

// -----------------------------------------------------------------------------
// --SECTION--                                             public static methods
// -----------------------------------------------------------------------------

////////////////////////////////////////////////////////////////////////////////
/// @addtogroup ArangoDB
/// @{
////////////////////////////////////////////////////////////////////////////////
      
      public:

////////////////////////////////////////////////////////////////////////////////
/// @brief exclude a collection from replication?
////////////////////////////////////////////////////////////////////////////////

        static bool excludeCollection (const char*);

////////////////////////////////////////////////////////////////////////////////
/// @brief filter a collection based on collection attributes
////////////////////////////////////////////////////////////////////////////////

        static bool filterCollection (struct TRI_vocbase_col_s*, void*);

////////////////////////////////////////////////////////////////////////////////
/// @}
////////////////////////////////////////////////////////////////////////////////

// -----------------------------------------------------------------------------
// --SECTION--                                                   private methods
// -----------------------------------------------------------------------------

////////////////////////////////////////////////////////////////////////////////
/// @addtogroup ArangoDB
/// @{
////////////////////////////////////////////////////////////////////////////////

      private:

////////////////////////////////////////////////////////////////////////////////
/// @brief add replication state to a JSON array
////////////////////////////////////////////////////////////////////////////////

        void addState (TRI_json_t*, struct TRI_replication_state_s const*);

////////////////////////////////////////////////////////////////////////////////
/// @brief remotely start the replication
////////////////////////////////////////////////////////////////////////////////

        void handleCommandStart ();

////////////////////////////////////////////////////////////////////////////////
/// @brief remotely stop the replication
////////////////////////////////////////////////////////////////////////////////

        void handleCommandStop ();

////////////////////////////////////////////////////////////////////////////////
/// @brief return the state of the replication
////////////////////////////////////////////////////////////////////////////////

        void handleCommandState ();

////////////////////////////////////////////////////////////////////////////////
/// @brief return the inventory (current replication and collection state)
////////////////////////////////////////////////////////////////////////////////

        void handleCommandInventory ();

////////////////////////////////////////////////////////////////////////////////
/// @brief handle a dump command for a specific collection
////////////////////////////////////////////////////////////////////////////////

        void handleCommandDump ();

////////////////////////////////////////////////////////////////////////////////
/// @}
////////////////////////////////////////////////////////////////////////////////

     };
  }
}

#endif

// Local Variables:
// mode: outline-minor
// outline-regexp: "/// @brief\\|/// {@inheritDoc}\\|/// @addtogroup\\|/// @page\\|// --SECTION--\\|/// @\\}"
// End:
