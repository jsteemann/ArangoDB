////////////////////////////////////////////////////////////////////////////////
/// @brief abstract base class for tasks
///
/// @file
/// Tasks are handled by the scheduler. The scheduler calls the task callback
/// as soon as a specific event occurs.
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
/// @author Dr. Frank Celler
/// @author Achim Brandt
/// @author Copyright 2014, ArangoDB GmbH, Cologne, Germany
/// @author Copyright 2008-2013, triAGENS GmbH, Cologne, Germany
////////////////////////////////////////////////////////////////////////////////

#ifndef ARANGODB_SCHEDULER_TASK_MANAGER_H
#define ARANGODB_SCHEDULER_TASK_MANAGER_H 1

#include "Basics/Common.h"

#include "Scheduler/events.h"

namespace triagens {
  namespace rest {
    class Task;
    class Scheduler;

////////////////////////////////////////////////////////////////////////////////
/// @ingroup Scheduler
/// @brief abstract base class for tasks
////////////////////////////////////////////////////////////////////////////////

    class TaskManager {
      private:
        TaskManager (TaskManager const&);
        TaskManager& operator= (TaskManager const&);

      public:

////////////////////////////////////////////////////////////////////////////////
/// @brief constructor
////////////////////////////////////////////////////////////////////////////////

        TaskManager () {
        }

////////////////////////////////////////////////////////////////////////////////
/// @brief destructor
////////////////////////////////////////////////////////////////////////////////

        virtual ~TaskManager () {
        }

      public:

////////////////////////////////////////////////////////////////////////////////
/// @brief deletes a task
////////////////////////////////////////////////////////////////////////////////

        void deleteTask (Task* task);

////////////////////////////////////////////////////////////////////////////////
/// @brief called to setup the callback information
///
/// Called to setup all the necessary information within the event loop. In case
/// of a multi-threaded scheduler the event loop identifier is supplied.
////////////////////////////////////////////////////////////////////////////////

        bool setupTask (Task*, Scheduler*, EventLoop loop);

////////////////////////////////////////////////////////////////////////////////
/// @brief called to clear the callback information
////////////////////////////////////////////////////////////////////////////////

        void cleanupTask (Task*);
    };
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
