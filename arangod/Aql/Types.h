////////////////////////////////////////////////////////////////////////////////
/// @brief fundamental types for the optimisation and execution of AQL
///
/// @file arangod/Aql/Types.h
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

#ifndef ARANGODB_AQL_TYPES_H
#define ARANGODB_AQL_TYPES_H 1

#include <functional>

#include <Basics/JsonHelper.h>
#include <BasicsC/json-utilities.h>

#include "Aql/AstNode.h"
#include "Aql/Variable.h"
#include "V8/v8-conv.h"
#include "VocBase/document-collection.h"
#include "VocBase/voc-shaper.h"
#include "Utils/V8TransactionContext.h"
#include "Utils/AqlTransaction.h"

namespace triagens {
  namespace aql {

// -----------------------------------------------------------------------------
// --SECTION--                                                            AqlDoc
// -----------------------------------------------------------------------------

////////////////////////////////////////////////////////////////////////////////
/// @brief type for register numbers/ids
////////////////////////////////////////////////////////////////////////////////

    typedef unsigned int RegisterId;

////////////////////////////////////////////////////////////////////////////////
/// @brief forward declaration for blocks of items
////////////////////////////////////////////////////////////////////////////////

    class AqlItemBlock;

////////////////////////////////////////////////////////////////////////////////
/// @brief a struct to hold a value, registers hole AqlValue* during the 
/// execution
////////////////////////////////////////////////////////////////////////////////

    struct AqlValue {

////////////////////////////////////////////////////////////////////////////////
/// @brief AqlValueType, indicates what sort of value we have
////////////////////////////////////////////////////////////////////////////////

      enum AqlValueType {
        EMPTY,     // contains no data
        JSON,      // Json*
        SHAPED,    // TRI_df_marker_t*
        DOCVEC,    // a vector of blocks of results coming from a subquery
        RANGE      // a pointer to a range remembering lower and upper bound
      };

////////////////////////////////////////////////////////////////////////////////
/// @brief Range, to hold a range compactly
////////////////////////////////////////////////////////////////////////////////

      struct Range {
        int64_t const _low;
        int64_t const _high;
        Range (int64_t low, int64_t high) : _low(low), _high(high) {}
      };

////////////////////////////////////////////////////////////////////////////////
/// @brief the actual data
////////////////////////////////////////////////////////////////////////////////

      union {
        triagens::basics::Json*     _json;
        TRI_df_marker_t const*      _marker;
        std::vector<AqlItemBlock*>* _vector;
        Range const*                _range;
      };
      
////////////////////////////////////////////////////////////////////////////////
/// @brief _type, the type of value
////////////////////////////////////////////////////////////////////////////////

      AqlValueType _type;

////////////////////////////////////////////////////////////////////////////////
/// @brief constructors for the various value types, note that they all take
/// ownership of the corresponding pointers
////////////////////////////////////////////////////////////////////////////////

      AqlValue () : _json(nullptr), _type(EMPTY) {
      }

      AqlValue (triagens::basics::Json* json)
        : _json(json), _type(JSON) {
      }
      
      AqlValue (TRI_df_marker_t const* marker)
        : _marker(marker), _type(SHAPED) {
      }
      
      AqlValue (std::vector<AqlItemBlock*>* vector)
        : _vector(vector), _type(DOCVEC) {
      }

      AqlValue (int64_t low, int64_t high) 
        : _type(RANGE) {
        _range = new Range(low, high);
      }

////////////////////////////////////////////////////////////////////////////////
/// @brief destructor, doing nothing automatically!
////////////////////////////////////////////////////////////////////////////////

      ~AqlValue () {
      }

////////////////////////////////////////////////////////////////////////////////
/// @brief destroy, explicit destruction, only when needed
////////////////////////////////////////////////////////////////////////////////

      void destroy ();

////////////////////////////////////////////////////////////////////////////////
/// @brief erase, this does not free the stuff in the AqlValue, it only
/// erases the pointers and makes the AqlValue structure EMPTY, this
/// is used when the AqlValue is stolen and stored in another object
////////////////////////////////////////////////////////////////////////////////

      void erase () {
        _type = EMPTY;
        _json = nullptr;
      }

////////////////////////////////////////////////////////////////////////////////
/// @brief clone for recursive copying
////////////////////////////////////////////////////////////////////////////////

      AqlValue clone () const;

////////////////////////////////////////////////////////////////////////////////
/// @brief construct a V8 value as input for the expression execution in V8
////////////////////////////////////////////////////////////////////////////////

      v8::Handle<v8::Value> toV8 (AQL_TRANSACTION_V8* trx, 
                                  TRI_document_collection_t const*) const;

////////////////////////////////////////////////////////////////////////////////
/// @brief toString method
////////////////////////////////////////////////////////////////////////////////

      std::string toString (TRI_document_collection_t const*) const;
      
////////////////////////////////////////////////////////////////////////////////
/// @brief toJson method
////////////////////////////////////////////////////////////////////////////////
      
      triagens::basics::Json toJson (TRI_document_collection_t const*) const;

////////////////////////////////////////////////////////////////////////////////
/// @brief a quick method to decide whether a value is empty
////////////////////////////////////////////////////////////////////////////////

      bool isEmpty () {
        return _type == EMPTY;
      }

////////////////////////////////////////////////////////////////////////////////
/// @brief a quick method to decide whether a value is true
////////////////////////////////////////////////////////////////////////////////

      bool isTrue () {
        if (_type != JSON) {
          return false;
        }
        TRI_json_t* json = _json->json();
        if (TRI_IsBooleanJson(json) && json->_value._boolean) {
          return true;
        }
        else if (TRI_IsNumberJson(json) && json->_value._number != 0.0) {
          return true;
        }
        else if (TRI_IsStringJson(json) && json->_value._string.length != 0) {
          return true;
        }
        else {
          return false;
        }
      }
    };

    int CompareAqlValues ( AqlValue const& left,  
                           TRI_document_collection_t const* leftcoll,
                           AqlValue const& right, 
                           TRI_document_collection_t const* rightcoll );
 
  } //closes namespace triagens::aql
}   //closes namespace triagens

////////////////////////////////////////////////////////////////////////////////
/// @brief hash function for AqlValue objects
////////////////////////////////////////////////////////////////////////////////

namespace std {

  template<> struct hash<triagens::aql::AqlValue> {
    size_t operator () (triagens::aql::AqlValue const& x) const {
      std::hash<uint32_t> intHash;
      std::hash<void const*> ptrHash;
      size_t res = intHash(static_cast<uint32_t>(x._type));
      switch (x._type) {
        case triagens::aql::AqlValue::EMPTY: {
          return res;
        }
        case triagens::aql::AqlValue::JSON: {
          return res ^ ptrHash(x._json);
        }
        case triagens::aql::AqlValue::SHAPED: {
          return res ^ ptrHash(x._marker);
        }
        case triagens::aql::AqlValue::DOCVEC: {
          return res ^ ptrHash(x._vector);
        }
        case triagens::aql::AqlValue::RANGE: {
          return res ^ ptrHash(x._range);
        }
        default: {
          TRI_ASSERT(false);
          return 0;
        }
      }
    }
  };

  template<> struct equal_to<triagens::aql::AqlValue> {
    bool operator () (triagens::aql::AqlValue const& a,
                      triagens::aql::AqlValue const& b) const {
      if (a._type != b._type) {
        return false;
      }
      switch (a._type) {
        case triagens::aql::AqlValue::JSON: {
          return a._json == b._json;
        }
        case triagens::aql::AqlValue::SHAPED: {
          return a._marker == b._marker;
        }
        case triagens::aql::AqlValue::DOCVEC: {
          return a._vector == b._vector;
        }
        case triagens::aql::AqlValue::RANGE: {
          return a._range == b._range;
        }
        default: {
          TRI_ASSERT(false);
          return true;
        }
      }
    }
  };

} //closes namespace std

namespace triagens {
  namespace aql {

// -----------------------------------------------------------------------------
// --SECTION--                                                      AqlItemBlock
// -----------------------------------------------------------------------------
// an <AqlItemBlock> is a <nrItems>x<nrRegs> vector of <AqlValue>s (not
// pointers). The size of an <AqlItemBlock> is the number of items.
// Entries in a given column (i.e. all the values of a given register
// for all items in the block) have the same type and belong to the
// same collection (if they are of type SHAPED). The document collection
// for a particular column is accessed via <getDocumentCollection>,
// and the entire array of document collections is accessed by
// <getDocumentCollections>. There is no access to an entire item, only
// access to particular registers of an item (via getValue).
//
// An AqlItemBlock is responsible to explicitly destroy all the
// <AqlValue>s it contains at destruction time. It is however allowed
// that multiple of the <AqlValue>s in it are pointing to identical
// structures, and thus must be destroyed only once for all identical
// copies. Furthermore, when parts of an AqlItemBlock are handed on
// to another AqlItemBlock, then the <AqlValue>s inside must be copied
// (deep copy) to make the blocks independent.

    class AqlItemBlock {

        std::vector<AqlValue> _data;
        std::vector<TRI_document_collection_t const*> _docColls;
        size_t     _nrItems;
        RegisterId _nrRegs;

////////////////////////////////////////////////////////////////////////////////
/// @brief the following is a means to say which pointers have already
/// been handed on to another block and therefore do must not be
/// freed. Set with "setHandedOn". Note that one should only set this
/// immediately before destruction, since otherwise it is not clear
/// whether the memory pointed to by the mentioned AqlValues is still
/// valid!
////////////////////////////////////////////////////////////////////////////////

        std::unordered_set<AqlValue>* _handedOn;

      public:

        AqlItemBlock (size_t nrItems, RegisterId nrRegs)
          : _nrItems(nrItems), _nrRegs(nrRegs), _handedOn(nullptr) {
          if (nrItems > 0 && nrRegs > 0) {
            _data.reserve(nrItems * nrRegs);
            for (size_t i = 0; i < nrItems * nrRegs; ++i) {
              _data.emplace_back();
            }
          }
          if (nrRegs > 0) {
            _docColls.reserve(nrRegs);
            for (size_t i = 0; i < nrRegs; ++i) {
              _docColls.push_back(nullptr);
            }
          }
        }

////////////////////////////////////////////////////////////////////////////////
/// @brief destructor
////////////////////////////////////////////////////////////////////////////////

        ~AqlItemBlock () {
          if (_handedOn == nullptr) {
            _handedOn = new std::unordered_set<AqlValue>();
          }
          for (size_t i = 0; i < _nrItems * _nrRegs; i++) {
            if (! _data[i].isEmpty()) {
              auto it = _handedOn->find(_data[i]);

              if (it == _handedOn->end()) {
                _handedOn->insert(_data[i]);
                _data[i].destroy();
              }
            }
          }
        }

////////////////////////////////////////////////////////////////////////////////
/// @brief
////////////////////////////////////////////////////////////////////////////////

        void setHandedOn (std::unordered_set<AqlValue>* handedOn) {
          TRI_ASSERT(_handedOn == nullptr);   // must set this only once
          _handedOn = handedOn;
        }

////////////////////////////////////////////////////////////////////////////////
/// @brief shrink the block to the specified number of rows, further
/// rows are erased without destroying the AqlValues in there!
////////////////////////////////////////////////////////////////////////////////

        void shrink (size_t nrItems) {
          if (nrItems == _nrItems) {
            // nothing to do
            return;
          }
          if (nrItems > _nrItems) {
            // cannot use shrink() to increase the size of the block
            THROW_ARANGO_EXCEPTION(TRI_ERROR_INTERNAL);
          }

          // erase all stored values in the region that we freed
          for (size_t i = nrItems; i < _nrItems; ++i) {
            for (RegisterId j = 0; j < _nrRegs; ++j) {
              eraseValue(i, j);
            }
          }

          // adjust the size of the block
          _nrItems = nrItems;
        }

////////////////////////////////////////////////////////////////////////////////
/// @brief getValue, get the value of a register
////////////////////////////////////////////////////////////////////////////////

        AqlValue getValue (size_t index, RegisterId varNr) const {
          return _data[index * _nrRegs + varNr];
        }

////////////////////////////////////////////////////////////////////////////////
/// @brief setValue, set the current value of a register
////////////////////////////////////////////////////////////////////////////////

      void setValue (size_t index, RegisterId varNr, AqlValue value) {
        TRI_ASSERT(_data.capacity() > index * _nrRegs + varNr);
        TRI_ASSERT(_data.at(index * _nrRegs + varNr).isEmpty());
        _data[index * _nrRegs + varNr] = value;
      }

////////////////////////////////////////////////////////////////////////////////
/// @brief eraseValue, erase the current value of a register not freeing it
/// this is used if the value is stolen and later released from elsewhere
////////////////////////////////////////////////////////////////////////////////

        void eraseValue (size_t index, RegisterId varNr) {
          _data[index * _nrRegs + varNr].erase();
        }

////////////////////////////////////////////////////////////////////////////////
/// @brief getDocumentCollection
////////////////////////////////////////////////////////////////////////////////

        TRI_document_collection_t const* getDocumentCollection (RegisterId varNr) const {
          return _docColls[varNr];
        }

////////////////////////////////////////////////////////////////////////////////
/// @brief setDocumentCollection, set the current value of a variable or attribute
////////////////////////////////////////////////////////////////////////////////

        void setDocumentCollection (RegisterId varNr, TRI_document_collection_t const* docColl) {
          _docColls[varNr] = docColl;
        }

////////////////////////////////////////////////////////////////////////////////
/// @brief getter for _nrRegs
////////////////////////////////////////////////////////////////////////////////

        RegisterId getNrRegs () const {
          return _nrRegs;
        }

////////////////////////////////////////////////////////////////////////////////
/// @brief getter for _nrItems
////////////////////////////////////////////////////////////////////////////////

        size_t size () const {
          return _nrItems;
        }

////////////////////////////////////////////////////////////////////////////////
/// @brief getter for _data
////////////////////////////////////////////////////////////////////////////////

        vector<AqlValue>& getData () {
          return _data;
        }

////////////////////////////////////////////////////////////////////////////////
/// @brief getter for _docColls
////////////////////////////////////////////////////////////////////////////////

        vector<TRI_document_collection_t const*>& getDocumentCollections () {
          return _docColls;
        }

////////////////////////////////////////////////////////////////////////////////
/// @brief slice/clone
////////////////////////////////////////////////////////////////////////////////

        AqlItemBlock* slice (size_t from, size_t to) {
          TRI_ASSERT(from < to && to <= _nrItems);

          std::unordered_map<AqlValue, AqlValue> cache;

          AqlItemBlock* res = nullptr;
          try {
            res = new AqlItemBlock(to - from, _nrRegs);

            for (RegisterId col = 0; col < _nrRegs; col++) {
              res->_docColls[col] = _docColls[col];
            }
            for (size_t row = from; row < to; row++) {
              for (RegisterId col = 0; col < _nrRegs; col++) {
                AqlValue& a(_data[row * _nrRegs + col]);

                if (! a.isEmpty()) {
                  auto it = cache.find(a);
                  if (it == cache.end()) {
                    AqlValue b = a.clone();
                    res->_data[(row - from) * _nrRegs + col] = b;
                    cache.insert(make_pair(a,b));
                  }
                  else {
                    res->_data[(row - from) * _nrRegs + col] = it->second;
                  }
                }
              }
            }

            return res;
          }
          catch (...) {
            delete res;
            throw;
          }
        }

////////////////////////////////////////////////////////////////////////////////
/// @brief slice/clone for a subset
////////////////////////////////////////////////////////////////////////////////

        AqlItemBlock* slice (vector<size_t>& chosen, size_t from, size_t to) {
          TRI_ASSERT(from < to && to <= chosen.size());

          std::unordered_map<AqlValue, AqlValue> cache;

          AqlItemBlock* res = nullptr;
          try {
            res = new AqlItemBlock(to - from, _nrRegs);

            for (RegisterId col = 0; col < _nrRegs; col++) {
              res->_docColls[col] = _docColls[col];
            }
            for (size_t row = from; row < to; row++) {
              for (RegisterId col = 0; col < _nrRegs; col++) {
                AqlValue& a(_data[chosen[row] * _nrRegs + col]);

                if (! a.isEmpty()) {
                  auto it = cache.find(a);
                  if (it == cache.end()) {
                    AqlValue b = a.clone();
                    res->_data[(row - from) * _nrRegs + col] = b;
                    cache.insert(make_pair(a,b));
                  }
                  else {
                    res->_data[(row - from) * _nrRegs + col] = it->second;
                  }
                }
              }
            }
          
            return res;
          }
          catch (...) {
            delete res;
            throw;
          }
        }

////////////////////////////////////////////////////////////////////////////////
/// @brief splice multiple blocks, note that the new block now owns all
/// AqlValue pointers in the old blocks, therefore, the latter are all
/// set to nullptr, just to be sure.
////////////////////////////////////////////////////////////////////////////////

        static AqlItemBlock* splice (std::vector<AqlItemBlock*>& blocks);

    };

  }  // namespace triagens::aql
}  // namespace triagens

#endif

// Local Variables:
// mode: outline-minor
// outline-regexp: "^\\(/// @brief\\|/// {@inheritDoc}\\|/// @addtogroup\\|// --SECTION--\\|/// @\\}\\)"
// End:

