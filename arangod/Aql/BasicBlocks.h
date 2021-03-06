////////////////////////////////////////////////////////////////////////////////
/// @brief AQL basic execution blocks
///
/// @file 
///
/// DISCLAIMER
///
/// Copyright 2010-2014 triagens GmbH, Cologne, Germany
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
/// @author Max Neunhoeffer
/// @author Copyright 2014, triagens GmbH, Cologne, Germany
////////////////////////////////////////////////////////////////////////////////

#ifndef ARANGODB_AQL_BASIC_BLOCKS_H
#define ARANGODB_AQL_BASIC_BLOCKS_H 1

#include "Aql/ExecutionBlock.h"
#include "Aql/ExecutionNode.h"
#include "Utils/AqlTransaction.h"

namespace triagens {
  namespace aql {

    class AqlItemBlock;

    class ExecutionEngine;

// -----------------------------------------------------------------------------
// --SECTION--                                                    SingletonBlock
// -----------------------------------------------------------------------------

    class SingletonBlock : public ExecutionBlock {

      void deleteInputVariables() {
        delete _inputRegisterValues;
        _inputRegisterValues = nullptr;
      }

      public:

        SingletonBlock (ExecutionEngine* engine, 
                        SingletonNode const* ep)
          : ExecutionBlock(engine, ep), 
            _inputRegisterValues(nullptr) {
        }

        ~SingletonBlock () {
          deleteInputVariables();
        }

        int initialize () override {
          _inputRegisterValues = nullptr;   // just in case
          return ExecutionBlock::initialize();
        }

////////////////////////////////////////////////////////////////////////////////
/// @brief initializeCursor, store a copy of the register values coming from above
////////////////////////////////////////////////////////////////////////////////

        int initializeCursor (AqlItemBlock* items, size_t pos) override;

        int shutdown (int) override final;

        bool hasMore () override final {
          return ! _done;
        }

        int64_t count () const override final {
          return 1;
        }

        int64_t remaining () override final {
          return _done ? 0 : 1;
        }

////////////////////////////////////////////////////////////////////////////////
/// @brief the bind data coming from outside
////////////////////////////////////////////////////////////////////////////////

      private:

        int getOrSkipSome (size_t atLeast,
                           size_t atMost,
                           bool skipping,
                           AqlItemBlock*& result,
                           size_t& skipped) override;

////////////////////////////////////////////////////////////////////////////////
/// @brief _inputRegisterValues
////////////////////////////////////////////////////////////////////////////////

        AqlItemBlock* _inputRegisterValues;

    };

// -----------------------------------------------------------------------------
// --SECTION--                                                       FilterBlock
// -----------------------------------------------------------------------------

    class FilterBlock : public ExecutionBlock {

      public:

        FilterBlock (ExecutionEngine*,
                     FilterNode const*);

        ~FilterBlock ();
        
        int initialize () override final;

      private:

////////////////////////////////////////////////////////////////////////////////
/// @brief internal function to actually decide if the document should be used
////////////////////////////////////////////////////////////////////////////////

        inline bool takeItem (AqlItemBlock* items, size_t index) const {
          return items->getValueReference(index, _inReg).isTrue();
        }

////////////////////////////////////////////////////////////////////////////////
/// @brief internal function to get another block
////////////////////////////////////////////////////////////////////////////////

        bool getBlock (size_t atLeast, size_t atMost);

        int getOrSkipSome (size_t atLeast,
                           size_t atMost,
                           bool skipping,
                           AqlItemBlock*& result,
                           size_t& skipped) override;

        bool hasMore () override final;

        int64_t count () const override final {
          return -1;   // refuse to work
        }

        int64_t remaining () override final {
          return -1;   // refuse to work
        }

////////////////////////////////////////////////////////////////////////////////
/// @brief input register
////////////////////////////////////////////////////////////////////////////////

      private:

        RegisterId _inReg;

////////////////////////////////////////////////////////////////////////////////
/// @brief vector of indices of those documents in the current block
/// that are chosen
////////////////////////////////////////////////////////////////////////////////

        std::vector<size_t> _chosen;

    };

// -----------------------------------------------------------------------------
// --SECTION--                                                        LimitBlock
// -----------------------------------------------------------------------------

    class LimitBlock : public ExecutionBlock {

      public:

        LimitBlock (ExecutionEngine* engine, 
                    LimitNode const* ep) 
          : ExecutionBlock(engine, ep), 
            _offset(ep->_offset), 
            _limit(ep->_limit),
            _count(0),
            _state(0), // start in the beginning
            _fullCount(ep->_fullCount) { 
        }

        ~LimitBlock () {
        }

        int initialize () override;

        int initializeCursor (AqlItemBlock* items, size_t pos) override final;

        virtual int getOrSkipSome (size_t atLeast,
                                   size_t atMost,
                                   bool skipping,
                                   AqlItemBlock*& result,
                                   size_t& skipped) override;

////////////////////////////////////////////////////////////////////////////////
/// @brief _offset
////////////////////////////////////////////////////////////////////////////////

        size_t _offset;

////////////////////////////////////////////////////////////////////////////////
/// @brief _limit
////////////////////////////////////////////////////////////////////////////////

        size_t _limit;

////////////////////////////////////////////////////////////////////////////////
/// @brief _count, number of items already handed on
////////////////////////////////////////////////////////////////////////////////

        size_t _count;

////////////////////////////////////////////////////////////////////////////////
/// @brief _state, 0 is beginning, 1 is after offset, 2 is done
////////////////////////////////////////////////////////////////////////////////

        int _state;

////////////////////////////////////////////////////////////////////////////////
/// @brief whether or not the block should count what it limits
////////////////////////////////////////////////////////////////////////////////

        bool const _fullCount;
    };

// -----------------------------------------------------------------------------
// --SECTION--                                                       ReturnBlock
// -----------------------------------------------------------------------------

    class ReturnBlock : public ExecutionBlock {

      public:

////////////////////////////////////////////////////////////////////////////////
/// @brief constructor
////////////////////////////////////////////////////////////////////////////////

        ReturnBlock (ExecutionEngine* engine,
                     ReturnNode const* ep)
          : ExecutionBlock(engine, ep),
            _returnInheritedResults(false) {

        }

////////////////////////////////////////////////////////////////////////////////
/// @brief destructor
////////////////////////////////////////////////////////////////////////////////

        ~ReturnBlock () {
        }

////////////////////////////////////////////////////////////////////////////////
/// @brief getSome
////////////////////////////////////////////////////////////////////////////////

        AqlItemBlock* getSome (size_t atLeast,
                               size_t atMost) override final;

////////////////////////////////////////////////////////////////////////////////
/// @brief make the return block return the results inherited from above, 
/// without creating new blocks
/// returns the id of the register the final result can be found in
////////////////////////////////////////////////////////////////////////////////

        RegisterId returnInheritedResults ();

// -----------------------------------------------------------------------------
// --SECTION--                                                 private variables
// -----------------------------------------------------------------------------

      private:

////////////////////////////////////////////////////////////////////////////////
/// @brief if set to true, the return block will return the AqlItemBlocks it
/// gets from above directly. if set to false, the return block will create a
/// new AqlItemBlock with one output register and copy the data from its input
/// block into it
////////////////////////////////////////////////////////////////////////////////

        bool _returnInheritedResults;

    };

// -----------------------------------------------------------------------------
// --SECTION--                                                    NoResultsBlock
// -----------------------------------------------------------------------------

    class NoResultsBlock : public ExecutionBlock {

      public:

        NoResultsBlock (ExecutionEngine* engine,
                        NoResultsNode const* ep)
          : ExecutionBlock(engine, ep) {
        }

        ~NoResultsBlock () {
        }

        int initialize () override {
          return ExecutionBlock::initialize();
        }

////////////////////////////////////////////////////////////////////////////////
/// @brief initializeCursor, store a copy of the register values coming from above
////////////////////////////////////////////////////////////////////////////////

        int initializeCursor (AqlItemBlock* items, size_t pos) override final;

        bool hasMore () override final {
          return false;
        }

        int64_t count () const override final {
          return 0;
        }

        int64_t remaining () override final {
          return 0;
        }

      private:

        int getOrSkipSome (size_t atLeast,
                           size_t atMost,
                           bool skipping,
                           AqlItemBlock*& result,
                           size_t& skipped) override;

    };

  }  // namespace triagens::aql
}  // namespace triagens

#endif

// Local Variables:
// mode: outline-minor
// outline-regexp: "^\\(/// @brief\\|/// {@inheritDoc}\\|/// @addtogroup\\|// --SECTION--\\|/// @\\}\\)"
// End:
