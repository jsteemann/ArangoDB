////////////////////////////////////////////////////////////////////////////////
/// @brief AQL SubqueryBlock
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

#include "SubqueryBlock.h"
#include "Aql/ExecutionEngine.h"
#include "Basics/Exceptions.h"
#include "VocBase/vocbase.h"

using namespace std;
using namespace triagens::arango;
using namespace triagens::aql;

// -----------------------------------------------------------------------------
// --SECTION--                                               class SubqueryBlock
// -----------------------------------------------------------------------------
        
SubqueryBlock::SubqueryBlock (ExecutionEngine* engine,
                              SubqueryNode const* en,
                              ExecutionBlock* subquery)
  : ExecutionBlock(engine, en), 
    _outReg(ExecutionNode::MaxRegisterId),
    _subquery(subquery) {
  
  auto it = en->getRegisterPlan()->varInfo.find(en->_outVariable->id);
  TRI_ASSERT(it != en->getRegisterPlan()->varInfo.end());
  _outReg = it->second.registerId;
  TRI_ASSERT(_outReg < ExecutionNode::MaxRegisterId);
}

////////////////////////////////////////////////////////////////////////////////
/// @brief destructor
////////////////////////////////////////////////////////////////////////////////

SubqueryBlock::~SubqueryBlock () {
}

////////////////////////////////////////////////////////////////////////////////
/// @brief initialize, tell dependency and the subquery
////////////////////////////////////////////////////////////////////////////////

int SubqueryBlock::initialize () {
  int res = ExecutionBlock::initialize();

  if (res != TRI_ERROR_NO_ERROR) {
    return res;
  }

  return getSubquery()->initialize();
}

////////////////////////////////////////////////////////////////////////////////
/// @brief getSome
////////////////////////////////////////////////////////////////////////////////

AqlItemBlock* SubqueryBlock::getSome (size_t atLeast,
                                      size_t atMost) {
  std::unique_ptr<AqlItemBlock> res(ExecutionBlock::getSomeWithoutRegisterClearout(atLeast, atMost));

  if (res.get() == nullptr) {
    return nullptr;
  }

  // TODO: constant and deterministic subqueries only need to be executed once
  bool const subqueryIsConst = false; // TODO 

  std::vector<AqlItemBlock*>* subqueryResults = nullptr;

  for (size_t i = 0; i < res->size(); i++) {
    int ret = _subquery->initializeCursor(res.get(), i);

    if (ret != TRI_ERROR_NO_ERROR) {
      THROW_ARANGO_EXCEPTION(ret);
    }

    if (i > 0 && subqueryIsConst) {
      // re-use already calculated subquery result
      TRI_ASSERT(subqueryResults != nullptr);
      res->setValue(i, _outReg, AqlValue(subqueryResults));
    }
    else {
      // initial subquery execution or subquery is not constant

      // execute the subquery
      subqueryResults = executeSubquery();
      TRI_ASSERT(subqueryResults != nullptr);

      try {
        TRI_IF_FAILURE("SubqueryBlock::getSome") {
          THROW_ARANGO_EXCEPTION(TRI_ERROR_DEBUG);
        }
        res->setValue(i, _outReg, AqlValue(subqueryResults));
      }
      catch (...) {
        destroySubqueryResults(subqueryResults);
        throw;
      }
    } 
      
    throwIfKilled(); // check if we were aborted
  }

  // Clear out registers no longer needed later:
  clearRegisters(res.get());
  return res.release();
}

////////////////////////////////////////////////////////////////////////////////
/// @brief shutdown, tell dependency and the subquery
////////////////////////////////////////////////////////////////////////////////

int SubqueryBlock::shutdown (int errorCode) {
  int res = ExecutionBlock::shutdown(errorCode);

  if (res != TRI_ERROR_NO_ERROR) {
    return res;
  }

  return getSubquery()->shutdown(errorCode);
}

////////////////////////////////////////////////////////////////////////////////
/// @brief execute the subquery and return its results
////////////////////////////////////////////////////////////////////////////////

std::vector<AqlItemBlock*>* SubqueryBlock::executeSubquery () {
  auto results = new std::vector<AqlItemBlock*>;

  try {
    do {
      std::unique_ptr<AqlItemBlock> tmp(_subquery->getSome(DefaultBatchSize, DefaultBatchSize));

      if (tmp.get() == nullptr) {
        break;
      }

      TRI_IF_FAILURE("SubqueryBlock::executeSubquery") {
        THROW_ARANGO_EXCEPTION(TRI_ERROR_DEBUG);
      }

      results->emplace_back(tmp.get());
      tmp.release();
    }
    while (true);
    return results;
  }
  catch (...) {
    destroySubqueryResults(results);
    throw;
  }
}

////////////////////////////////////////////////////////////////////////////////
/// @brief destroy the results of a subquery
////////////////////////////////////////////////////////////////////////////////

void SubqueryBlock::destroySubqueryResults (std::vector<AqlItemBlock*>* results) {
  for (auto& x : *results) {
    delete x;
  }
  delete results;
}

// Local Variables:
// mode: outline-minor
// outline-regexp: "^\\(/// @brief\\|/// {@inheritDoc}\\|/// @addtogroup\\|// --SECTION--\\|/// @\\}\\)"
// End:
