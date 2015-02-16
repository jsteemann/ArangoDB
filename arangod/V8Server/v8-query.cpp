////////////////////////////////////////////////////////////////////////////////
/// @brief V8-vocbase queries
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
/// @author Dr. Frank Celler
/// @author Copyright 2014, ArangoDB GmbH, Cologne, Germany
/// @author Copyright 2011-2013, triAGENS GmbH, Cologne, Germany
////////////////////////////////////////////////////////////////////////////////

#include "v8-query.h"

#include "Basics/logging.h"
#include "Basics/random.h"
#include "Basics/string-buffer.h"
#include "GeoIndex/geo-index.h"
#include "HashIndex/hash-index.h"
#include "FulltextIndex/fulltext-index.h"
#include "FulltextIndex/fulltext-result.h"
#include "FulltextIndex/fulltext-query.h"
#include "Mvcc/CollectionOperations.h"
#include "Mvcc/EdgeIndex.h"
#include "Mvcc/GeoIndex2.h"
#include "Mvcc/HashIndex.h"
#include "Mvcc/MasterpointerManager.h"
#include "Mvcc/Transaction.h"
#include "Mvcc/TransactionCollection.h"
#include "Mvcc/TransactionScope.h"
#include "SkipLists/skiplistIndex.h"
#include "Utils/transactions.h"
#include "V8/v8-globals.h"
#include "V8/v8-conv.h"
#include "V8/v8-utils.h"
#include "V8Server/v8-vocbase.h"
#include "VocBase/edge-collection.h"
#include "VocBase/vocbase.h"
#include "V8Server/v8-vocindex.h"
#include "V8Server/v8-wrapshapedjson.h"

using namespace std;
using namespace triagens::basics;
using namespace triagens::arango;

// -----------------------------------------------------------------------------
// --SECTION--                                                  helper functions
// -----------------------------------------------------------------------------

////////////////////////////////////////////////////////////////////////////////
/// @brief extracts skip and limit
////////////////////////////////////////////////////////////////////////////////

static void ExtractSkipAndLimit (const v8::FunctionCallbackInfo<v8::Value>& args,
                                 size_t pos,
                                 TRI_voc_ssize_t& skip,
                                 TRI_voc_size_t& limit) {

  skip = TRI_QRY_NO_SKIP;
  limit = TRI_QRY_NO_LIMIT;

  if (pos < (size_t) args.Length() && ! args[(int) pos]->IsNull() && ! args[(int) pos]->IsUndefined()) {
    skip = (TRI_voc_ssize_t) TRI_ObjectToDouble(args[(int) pos]);
  }

  if (pos + 1 < (size_t) args.Length() && ! args[(int) pos + 1]->IsNull() && ! args[(int) pos + 1]->IsUndefined()) {
    limit = (TRI_voc_size_t) TRI_ObjectToDouble(args[(int) pos + 1]);
  }
}

////////////////////////////////////////////////////////////////////////////////
/// @brief calculates slice
////////////////////////////////////////////////////////////////////////////////

static void CalculateSkipLimitSlice (size_t length,
                                     TRI_voc_ssize_t skip,
                                     TRI_voc_size_t limit,
                                     size_t& s,
                                     size_t& e) {
  s = 0;
  e = length;

  // skip from the beginning
  if (0 < skip) {
    s = (size_t) skip;

    if (e < s) {
      s = (size_t) e;
    }
  }

  // skip from the end
  else if (skip < 0) {
    skip = -skip;

    if ((size_t) skip < e) {
      s = e - skip;
    }
  }

  // apply limit
  if (s + limit < e) {
    int64_t sum = (int64_t) s + (int64_t) limit;
    if (sum < (int64_t) e) {
      if (sum >= (int64_t) TRI_QRY_NO_LIMIT) {
        e = TRI_QRY_NO_LIMIT;
      }
      else {
        e = (size_t) sum;
      }
    }
  }
}

// -----------------------------------------------------------------------------
// --SECTION--                                                     random access
// -----------------------------------------------------------------------------

////////////////////////////////////////////////////////////////////////////////
/// @brief selects a random document
///
/// @FUN{@FA{collection}.any()}
///
/// The @FN{any} method returns a random document from the collection.  It returns
/// @LIT{null} if the collection is empty.
///
/// @EXAMPLES
///
/// @code
/// arangod> db.example.any()
/// { "_id" : "example/222716379559", "_rev" : "222716379559", "Hello" : "World" }
/// @endcode
////////////////////////////////////////////////////////////////////////////////

static void JS_MvccAny (const v8::FunctionCallbackInfo<v8::Value>& args) {
  TransactionBase transBase(true);   // To protect against assertions, FIXME later
  v8::Isolate* isolate = args.GetIsolate();
  v8::HandleScope scope(isolate);

  auto const* collection = TRI_UnwrapClass<TRI_vocbase_col_t>(args.Holder(), TRI_GetVocBaseColType());

  if (collection == nullptr) {
    TRI_V8_THROW_EXCEPTION_INTERNAL("cannot extract collection");
  }
    
  CollectionNameResolver resolver(collection->_vocbase); // TODO
  
  try {
    triagens::mvcc::TransactionScope transactionScope(collection->_vocbase, triagens::mvcc::TransactionScope::NoCollections());

    auto* transaction = transactionScope.transaction();
    auto* transactionCollection = transaction->collection(collection->_cid);
  
    auto readResult = triagens::mvcc::CollectionOperations::RandomDocument(&transactionScope, transactionCollection);
    
    if (readResult.code != TRI_ERROR_NO_ERROR) {
      THROW_ARANGO_EXCEPTION(readResult.code);
    }

    if (readResult.mptr == nullptr) {
      // collection is empty
      TRI_V8_RETURN_NULL();
    }

    // convert to v8
    v8::Handle<v8::Value> result = TRI_WrapShapedJson(isolate, &resolver, transactionCollection, readResult.mptr->getDataPtr());

    if (result.IsEmpty()) {
      TRI_V8_THROW_EXCEPTION(TRI_ERROR_OUT_OF_MEMORY);
    }

    TRI_V8_RETURN(result);
  }
  catch (triagens::arango::Exception const& ex) {
    TRI_V8_THROW_EXCEPTION(ex.code());
  }
  catch (...) {
    TRI_V8_THROW_EXCEPTION(TRI_ERROR_INTERNAL);
  }

  TRI_ASSERT(false);
}


// -----------------------------------------------------------------------------
// --SECTION--                                                  by example query
// -----------------------------------------------------------------------------

////////////////////////////////////////////////////////////////////////////////
/// @brief destroys the example object
////////////////////////////////////////////////////////////////////////////////

static void DestroyExample (std::vector<TRI_shaped_json_t*>& values) {
  for (auto it : values) {
    if (it != nullptr) {
      TRI_FreeShapedJson(TRI_UNKNOWN_MEM_ZONE, it);
    }
  }

  values.clear();
}

////////////////////////////////////////////////////////////////////////////////
/// @brief sets up the example object
////////////////////////////////////////////////////////////////////////////////

static int SetupExample (v8::Isolate* isolate,
                         std::vector<TRI_shape_pid_t>& pids,
                         std::vector<TRI_shaped_json_t*>& values,
                         v8::Handle<v8::Object> const example,
                         TRI_shaper_t* shaper) {

  // get own properties of example
  v8::Handle<v8::Array> names = example->GetOwnPropertyNames();
  size_t n = names->Length();

  pids.reserve(n);
  values.reserve(n);

  // convert
  for (size_t i = 0;  i < n;  ++i) {
    v8::Handle<v8::Value> key = names->Get((uint32_t) i);
    v8::Handle<v8::Value> val = example->Get(key);

    // property initialise the memory
    values[i] = 0;

    TRI_Utf8ValueNFC keyStr(TRI_UNKNOWN_MEM_ZONE, key);

    if (*keyStr == nullptr) {
      return TRI_ERROR_BAD_PARAMETER;
    }

    auto pid = shaper->lookupAttributePathByName(shaper, *keyStr);

    if (pid == 0) {
      // no attribute path found. this means the result will be empty
      return TRI_RESULT_ELEMENT_NOT_FOUND;
    }

    pids.push_back(pid);

    auto shaped = TRI_ShapedJsonV8Object(isolate, val, shaper, false);

    if (shaped == nullptr) {
      return TRI_RESULT_ELEMENT_NOT_FOUND;
    }

    try {
      values.push_back(shaped);
    }
    catch (...) {
      TRI_FreeShapedJson(shaper->_memoryZone, shaped);
      throw;
    }
  }

  return TRI_ERROR_NO_ERROR;
}

////////////////////////////////////////////////////////////////////////////////
/// @brief selects documents by example, full collection scan
////////////////////////////////////////////////////////////////////////////////

static void JS_MvccByExample (const v8::FunctionCallbackInfo<v8::Value>& args) {
  TransactionBase transBase(true);   // To protect against assertions, FIXME later
  v8::Isolate* isolate = args.GetIsolate();
  v8::HandleScope scope(isolate);

  auto const* collection = TRI_UnwrapClass<TRI_vocbase_col_t>(args.Holder(), TRI_GetVocBaseColType());

  if (collection == nullptr) {
    TRI_V8_THROW_EXCEPTION_INTERNAL("cannot extract collection");
  }

  TRI_THROW_SHARDING_COLLECTION_NOT_YET_IMPLEMENTED(collection);

  // expecting index, example, skip, and limit
  if (args.Length() < 1) {
    TRI_V8_THROW_EXCEPTION_USAGE("mvccByExample(<example>, <skip>, <limit>)");
  }

  // extract the example
  if (! args[0]->IsObject()) {
    TRI_V8_THROW_TYPE_ERROR("<example> must be an object");
  }

  v8::Handle<v8::Object> example = args[0]->ToObject();

  // extract skip and limit
  TRI_voc_ssize_t skip;
  TRI_voc_size_t limit;
  ExtractSkipAndLimit(args, 1, skip, limit);
  
  triagens::mvcc::SearchOptions searchOptions;
  searchOptions.skip    = skip;
  searchOptions.limit   = limit;
  
  triagens::mvcc::OperationOptions options;
  options.searchOptions = &searchOptions;
    
  CollectionNameResolver resolver(collection->_vocbase); // TODO

  try {
    triagens::mvcc::TransactionScope transactionScope(collection->_vocbase, triagens::mvcc::TransactionScope::NoCollections());

    auto* transaction = transactionScope.transaction();
    auto* transactionCollection = transaction->collection(collection->_cid);
  
    std::vector<TRI_shape_pid_t> pids;
    std::vector<TRI_shaped_json_t*> values;

    int res = SetupExample(isolate, pids, values, example, transactionCollection->shaper());

    if (res != TRI_ERROR_NO_ERROR) {
      DestroyExample(values);

      if (res == TRI_RESULT_ELEMENT_NOT_FOUND) {
        TRI_V8_RETURN(v8::Array::New(isolate));
      }
      TRI_V8_THROW_EXCEPTION(res);
    }

    std::vector<TRI_doc_mptr_t const*> foundDocuments;
    auto searchResult = triagens::mvcc::CollectionOperations::ReadByExample(&transactionScope, transactionCollection, pids, values, foundDocuments, options);
    DestroyExample(values);
 
    if (searchResult.code != TRI_ERROR_NO_ERROR) {
      THROW_ARANGO_EXCEPTION(searchResult.code);
    }
      
    size_t const n = foundDocuments.size();
    auto result = v8::Array::New(isolate, static_cast<int>(n));

    for (size_t i = 0; i < n; ++i) {
      v8::Handle<v8::Value> document = TRI_WrapShapedJson(isolate, &resolver, transactionCollection, foundDocuments[i]->getDataPtr());

      if (document.IsEmpty()) {
        TRI_V8_THROW_EXCEPTION_MEMORY();
      }
      result->Set(static_cast<uint32_t>(i), document);
    }

    TRI_V8_RETURN(result);
  }
  catch (triagens::arango::Exception const& ex) {
    TRI_V8_THROW_EXCEPTION(ex.code());
  }
  catch (...) {
    TRI_V8_THROW_EXCEPTION(TRI_ERROR_INTERNAL);
  }

  TRI_ASSERT(false);
}

// -----------------------------------------------------------------------------
// --SECTION--                                                  hash index query
// -----------------------------------------------------------------------------

////////////////////////////////////////////////////////////////////////////////
/// @brief destroys the example object for a hash index query
////////////////////////////////////////////////////////////////////////////////

static void DestroySearchValue (std::vector<TRI_shaped_json_t>& searchValue) {
  for (auto& it : searchValue) {
    TRI_DestroyShapedJson(TRI_UNKNOWN_MEM_ZONE, &it);
  }
  searchValue.clear();
}

////////////////////////////////////////////////////////////////////////////////
/// @brief sets up the example object for a hash index query
////////////////////////////////////////////////////////////////////////////////

static int SetupSearchValue (v8::Isolate* isolate,
                             std::vector<TRI_shape_pid_t> const& paths,
                             v8::Handle<v8::Object> const example,
                             TRI_shaper_t* shaper,
                             std::vector<TRI_shaped_json_t>& result) {
  TRI_ASSERT(result.size() == paths.size());
  TRI_ASSERT(! paths.empty());

  for (size_t i = 0; i < paths.size(); ++i) {
    auto pid = paths[i];
    TRI_ASSERT(pid != 0);
    char const* name = TRI_AttributeNameShapePid(shaper, pid);

    if (name == nullptr) {
      return TRI_ERROR_BAD_PARAMETER;
    }

    v8::Handle<v8::String> key = TRI_V8_STRING(name);
    int res;

    if (example->HasOwnProperty(key)) {
      res = TRI_FillShapedJsonV8Object(isolate, example->Get(key), &result[i], shaper, false);
    }
    else {
      res = TRI_FillShapedJsonV8Object(isolate, v8::Null(isolate), &result[i], shaper, false);
    }

    if (res != TRI_ERROR_NO_ERROR) {
      return res;
    }
  }

  return TRI_ERROR_NO_ERROR;
}

////////////////////////////////////////////////////////////////////////////////
/// @brief selects documents by example using a hash index
////////////////////////////////////////////////////////////////////////////////

static void JS_MvccByExampleHashIndex (const v8::FunctionCallbackInfo<v8::Value>& args) {
  TransactionBase transBase(true);   // To protect against assertions, FIXME later
  v8::Isolate* isolate = args.GetIsolate();
  v8::HandleScope scope(isolate);

  auto const* collection = TRI_UnwrapClass<TRI_vocbase_col_t>(args.Holder(), TRI_GetVocBaseColType());

  if (collection == nullptr) {
    TRI_V8_THROW_EXCEPTION_INTERNAL("cannot extract collection");
  }

  TRI_THROW_SHARDING_COLLECTION_NOT_YET_IMPLEMENTED(collection);

  // expecting index, example, skip, and limit
  if (args.Length() < 2) {
    TRI_V8_THROW_EXCEPTION_USAGE("mvccByExampleHash(<index>, <example>, <skip>, <limit>)");
  }

  // extract the example
  if (! args[1]->IsObject()) {
    TRI_V8_THROW_TYPE_ERROR("<example> must be an object");
  }

  v8::Handle<v8::Object> example = args[1]->ToObject();

  // extract skip and limit
  TRI_voc_ssize_t skip;
  TRI_voc_size_t limit;
  ExtractSkipAndLimit(args, 2, skip, limit);

  try {
    triagens::mvcc::TransactionScope transactionScope(collection->_vocbase, triagens::mvcc::TransactionScope::NoCollections());

    auto* transaction = transactionScope.transaction();
    auto* transactionCollection = transaction->collection(collection->_cid);
  
    // extract the index
    CollectionNameResolver resolver(collection->_vocbase); // TODO
    auto index = TRI_LookupMvccIndexByHandle(isolate, &resolver, collection, args[0]);

    if (index == nullptr ||
        index->type() != TRI_IDX_TYPE_HASH_INDEX) {
      TRI_V8_THROW_EXCEPTION(TRI_ERROR_ARANGO_NO_INDEX);
    }

    auto hashIndex = static_cast<triagens::mvcc::HashIndex*>(index);
    auto const& paths = hashIndex->paths();

    std::vector<TRI_shaped_json_t> searchValue(paths.size());

    int res = SetupSearchValue(isolate, paths, example, transactionCollection->shaper(), searchValue);

    if (res != TRI_ERROR_NO_ERROR) {
      DestroySearchValue(searchValue);

      if (res == TRI_RESULT_ELEMENT_NOT_FOUND) {
        TRI_V8_RETURN(v8::Array::New(isolate));
      }
      TRI_V8_THROW_EXCEPTION(res);
    }

    // setup result
    std::unique_ptr<std::vector<TRI_doc_mptr_t*>> indexResult(hashIndex->lookup(transactionCollection, transaction, &searchValue, 0)); 
    DestroySearchValue(searchValue);
 
    if (indexResult.get() == nullptr) {
      TRI_V8_RETURN(v8::Array::New(isolate));
    }

    auto const& foundDocuments = *(indexResult.get());

    size_t total = indexResult->size();
    size_t count = 0;

    auto result = v8::Array::New(isolate);

    if (total > 0) {
      size_t s;
      size_t e;
      CalculateSkipLimitSlice(total, skip, limit, s, e);

      if (s < e) {
        for (size_t i = s;  i < e;  ++i) {
          v8::Handle<v8::Value> document = TRI_WrapShapedJson(isolate, &resolver, transactionCollection, foundDocuments[i]->getDataPtr());

          if (document.IsEmpty()) {
            TRI_V8_THROW_EXCEPTION(TRI_ERROR_OUT_OF_MEMORY);
            break;
          }

          result->Set(static_cast<uint32_t>(count++), document);
        }
      }
    }
  
    TRI_V8_RETURN(result);
  }
  catch (triagens::arango::Exception const& ex) {
    TRI_V8_THROW_EXCEPTION(ex.code());
  }
  catch (...) {
    TRI_V8_THROW_EXCEPTION(TRI_ERROR_INTERNAL);
  }

  TRI_ASSERT(false);
}

// -----------------------------------------------------------------------------
// --SECTION--                                                    checksum query
// -----------------------------------------------------------------------------

////////////////////////////////////////////////////////////////////////////////
/// @brief helper struct used when calculting checksums
////////////////////////////////////////////////////////////////////////////////

struct CollectionChecksumHelper {
  CollectionChecksumHelper (CollectionNameResolver const* resolver,
                            TRI_shaper_t* shaper,
                            bool withRevisions,
                            bool withData)
    : resolver(resolver),
      shaper(shaper),
      checksum(0),
      withRevisions(withRevisions),
      withData(withData) {
      
    TRI_InitStringBuffer(&buffer, TRI_UNKNOWN_MEM_ZONE);
  }
  
  ~CollectionChecksumHelper () {
    TRI_DestroyStringBuffer(&buffer);
  }

  void update (TRI_doc_mptr_t const* mptr) {
    TRI_df_marker_t const* marker = static_cast<TRI_df_marker_t const*>(mptr->getDataPtr());  // PROTECTED by trx in calling function TRI_DocumentIteratorDocumentCollection
    uint32_t localCrc;

    if (marker->_type == TRI_DOC_MARKER_KEY_DOCUMENT ||
        marker->_type == TRI_WAL_MARKER_DOCUMENT ||
        marker->_type == TRI_WAL_MARKER_MVCC_DOCUMENT) {
      localCrc = TRI_Crc32HashString(TRI_EXTRACT_MARKER_KEY(mptr));  // PROTECTED by trx in calling function TRI_DocumentIteratorDocumentCollection
      if (withRevisions) {
        localCrc += TRI_Crc32HashPointer(&mptr->_rid, sizeof(TRI_voc_rid_t));
      }
    }
    else if (marker->_type == TRI_DOC_MARKER_KEY_EDGE ||
             marker->_type == TRI_WAL_MARKER_EDGE ||
             marker->_type == TRI_WAL_MARKER_MVCC_EDGE) {
      TRI_voc_cid_t fromCid;
      TRI_voc_cid_t toCid;
      uint32_t offsetFromKey;
      uint32_t offsetToKey;

      // must convert _rid, _fromCid, _toCid into strings for portability
      localCrc = TRI_Crc32HashString(TRI_EXTRACT_MARKER_KEY(mptr));  // PROTECTED by trx in calling function TRI_DocumentIteratorDocumentCollection
      if (withRevisions) {
        localCrc += TRI_Crc32HashPointer(&mptr->_rid, sizeof(TRI_voc_rid_t));
      }

      if (marker->_type == TRI_DOC_MARKER_KEY_EDGE) {
        auto* e = reinterpret_cast<TRI_doc_edge_key_marker_t const*>(marker);
        fromCid       = e->_fromCid;
        toCid         = e->_toCid;
        offsetFromKey = e->_offsetFromKey;
        offsetToKey   = e->_offsetToKey;
      }
      else if (marker->_type == TRI_WAL_MARKER_EDGE) {
        auto* e = reinterpret_cast<triagens::wal::edge_marker_t const*>(marker);
        fromCid       = e->_fromCid;
        toCid         = e->_toCid;
        offsetFromKey = e->_offsetFromKey;
        offsetToKey   = e->_offsetToKey;
      }
      else {
        auto* e = reinterpret_cast<triagens::wal::mvcc_edge_marker_t const*>(marker);
        fromCid       = e->_fromCid;
        toCid         = e->_toCid;
        offsetFromKey = e->_offsetFromKey;
        offsetToKey   = e->_offsetToKey;
      }
        
      string const extra = resolver->getCollectionNameCluster(toCid) + 
                           TRI_DOCUMENT_HANDLE_SEPARATOR_CHR + 
                           string(((char*) marker) + offsetToKey) +
                           resolver->getCollectionNameCluster(fromCid) + 
                           TRI_DOCUMENT_HANDLE_SEPARATOR_CHR + 
                           string(((char*) marker) + offsetFromKey);
 
      localCrc += TRI_Crc32HashPointer(extra.c_str(), extra.size());
    }
    else {
      return;
    } 

    if (withData) {
      // with data
      void const* d = static_cast<void const*>(marker);

      TRI_shaped_json_t shaped;
      TRI_EXTRACT_SHAPED_JSON_MARKER(shaped, d);

      TRI_StringifyArrayShapedJson(shaper, &buffer, &shaped, false);  // PROTECTED by trx in calling function TRI_DocumentIteratorDocumentCollection
      localCrc += TRI_Crc32HashPointer(TRI_BeginStringBuffer(&buffer), TRI_LengthStringBuffer(&buffer));
      TRI_ResetStringBuffer(&buffer);
    }

    checksum += localCrc;
  }

  CollectionNameResolver const*  resolver;
  TRI_shaper_t*                  shaper;
  TRI_string_buffer_t            buffer;
  uint32_t                       checksum;
  bool const                     withRevisions;
  bool const                     withData;
    
};

////////////////////////////////////////////////////////////////////////////////
/// @brief calculates a checksum for the data in a collection
/// @startDocuBlock collectionChecksum
/// `collection.checksum(withRevisions, withData)`
///
/// The *checksum* operation calculates a CRC32 checksum of the keys
/// contained in collection *collection*.
///
/// If the optional argument *withRevisions* is set to *true*, then the
/// revision ids of the documents are also included in the checksumming.
///
/// If the optional argument *withData* is set to *true*, then the
/// actual document data is also checksummed. Including the document data in
/// checksumming will make the calculation slower, but is more accurate.
///
/// **Note**: this method is not available in a cluster.
///
/// @endDocuBlock
////////////////////////////////////////////////////////////////////////////////

static void JS_MvccChecksum (const v8::FunctionCallbackInfo<v8::Value>& args) {
  TransactionBase transBase(true);   // To protect against assertions, FIXME later
  v8::Isolate* isolate = args.GetIsolate();
  v8::HandleScope scope(isolate);

  auto const* collection = TRI_UnwrapClass<TRI_vocbase_col_t>(args.Holder(), TRI_GetVocBaseColType());

  if (collection == nullptr) {
    TRI_V8_THROW_EXCEPTION_INTERNAL("cannot extract collection");
  }
  
  if (ServerState::instance()->isCoordinator()) {
    // renaming a collection in a cluster is unsupported
    TRI_V8_THROW_EXCEPTION(TRI_ERROR_CLUSTER_UNSUPPORTED);
  }

  bool withRevisions = false;
  if (args.Length() > 0) {
    withRevisions = TRI_ObjectToBoolean(args[0]);
  }

  bool withData = false;
  if (args.Length() > 1) {
    withData = TRI_ObjectToBoolean(args[1]);
  }

  try {
    triagens::mvcc::TransactionScope transactionScope(collection->_vocbase, triagens::mvcc::TransactionScope::NoCollections());

    auto* transaction = transactionScope.transaction();
    auto* transactionCollection = transaction->collection(collection->_cid);
  
    // extract the index
    CollectionNameResolver resolver(collection->_vocbase); // TODO
    CollectionChecksumHelper helper(&resolver, transactionCollection->shaper(), withRevisions, withData);
  
    {
      std::unique_ptr<triagens::mvcc::MasterpointerIterator> iterator(new triagens::mvcc::MasterpointerIterator(transaction, transactionCollection->masterpointerManager(), false));
    
      auto it = iterator.get();
      while (it->hasMore()) {
        TRI_doc_mptr_t const* found = it->next();
        TRI_ASSERT(found != nullptr);

        helper.update(found);
      }
    }
 
    string const revisionString(std::to_string(triagens::mvcc::CollectionOperations::Revision(&transactionScope, transactionCollection)));

    v8::Handle<v8::Object> result = v8::Object::New(isolate);
    result->Set(TRI_V8_ASCII_STRING("checksum"), v8::Number::New(isolate, helper.checksum));
    result->Set(TRI_V8_ASCII_STRING("revision"), TRI_V8_STD_STRING(revisionString));

    TRI_V8_RETURN(result);
  }
  catch (triagens::arango::Exception const& ex) {
    TRI_V8_THROW_EXCEPTION(ex.code());
  }
  catch (...) {
    TRI_V8_THROW_EXCEPTION(TRI_ERROR_INTERNAL);
  }

  TRI_ASSERT(false);
}


// -----------------------------------------------------------------------------
// --SECTION--                                                     edges queries
// -----------------------------------------------------------------------------

////////////////////////////////////////////////////////////////////////////////
/// @brief worker function for querying the edge index
////////////////////////////////////////////////////////////////////////////////
    
static int AddEdges (v8::Isolate* isolate, 
                     TRI_edge_direction_e direction, 
                     triagens::mvcc::Transaction* transaction,
                     triagens::mvcc::TransactionCollection* transactionCollection,
                     CollectionNameResolver const* resolver,
                     triagens::mvcc::EdgeIndex* edgeIndex,
                     v8::Handle<v8::Array>& result,
                     v8::Handle<v8::Value> const vertex) {
  TRI_voc_cid_t cid;
  std::unique_ptr<char[]> key;

  int res = TRI_ParseVertex(isolate, resolver, cid, key, vertex);

  if (res != TRI_ERROR_NO_ERROR) {
    return res;
  }

  uint32_t i = result->Length();

  TRI_edge_header_t const edge = { cid, key.get() }; 
  std::unique_ptr<std::vector<TRI_doc_mptr_t*>> found(edgeIndex->lookup(transaction, direction, &edge, 0));

  if (found.get() == nullptr) {
    return TRI_ERROR_NO_ERROR;
  }

  auto edges = (*found.get());

  if (i == 0 && ! edges.empty()) {
    result = v8::Array::New(isolate, static_cast<uint32_t>(edges.size()));
  }

  for (auto const& it : edges) {
    v8::Handle<v8::Value> document = TRI_WrapShapedJson(isolate, resolver, transactionCollection, it->getDataPtr());

    if (document.IsEmpty()) {
      return TRI_ERROR_OUT_OF_MEMORY;
    }
          
    result->Set(i++, document);
  }

  return TRI_ERROR_NO_ERROR;
}
  
////////////////////////////////////////////////////////////////////////////////
/// @brief queries the edge index
////////////////////////////////////////////////////////////////////////////////

static void MvccEdgesQuery (TRI_edge_direction_e direction,
                            const v8::FunctionCallbackInfo<v8::Value>& args) {
  TransactionBase transBase(true);   // To protect against assertions, FIXME later
  v8::Isolate* isolate = args.GetIsolate();
  v8::HandleScope scope(isolate);

  auto const* collection = TRI_UnwrapClass<TRI_vocbase_col_t>(args.Holder(), TRI_GetVocBaseColType());

  if (collection == nullptr) {
    TRI_V8_THROW_EXCEPTION_INTERNAL("cannot extract collection");
  }

  TRI_THROW_SHARDING_COLLECTION_NOT_YET_IMPLEMENTED(collection);
  
  if (collection->_type != TRI_COL_TYPE_EDGE) {
    TRI_V8_THROW_EXCEPTION(TRI_ERROR_ARANGO_COLLECTION_TYPE_INVALID);
  }
  
  // first and only argument schould be a list of document idenfifier
  if (args.Length() != 1) {
    switch (direction) {
      case TRI_EDGE_IN:
        TRI_V8_THROW_EXCEPTION_USAGE("mvccInEdges(<vertices>)");

      case TRI_EDGE_OUT:
        TRI_V8_THROW_EXCEPTION_USAGE("mvccOutEdges(<vertices>)");

      case TRI_EDGE_ANY:
      default: {
        TRI_V8_THROW_EXCEPTION_USAGE("mvccEdges(<vertices>)");
      }
    }
  }
    
  CollectionNameResolver resolver(collection->_vocbase); // TODO

  try {
    triagens::mvcc::TransactionScope transactionScope(collection->_vocbase, triagens::mvcc::TransactionScope::NoCollections());

    auto* transaction = transactionScope.transaction();
    auto* transactionCollection = transaction->collection(collection->_cid);
  
    auto edgeIndex = static_cast<triagens::mvcc::EdgeIndex*>(transactionCollection->documentCollection()->lookupIndex(TRI_IDX_TYPE_EDGE_INDEX));

    if (edgeIndex == nullptr) {
      // collection must have an edge index
      TRI_V8_THROW_EXCEPTION(TRI_ERROR_INTERNAL);
    }

    v8::Handle<v8::Array> result = v8::Array::New(isolate);

    if (args[0]->IsArray()) {
      v8::Handle<v8::Array> vertices = v8::Handle<v8::Array>::Cast(args[0]);
      uint32_t const length = vertices->Length();

      for (uint32_t i = 0; i < length; ++i) {
        int res = AddEdges(isolate, direction, transaction, transactionCollection, &resolver, edgeIndex, result, vertices->Get(i));

        if (res != TRI_ERROR_NO_ERROR) {
          // ignore error
          continue;
        }
      }
    }
    else {
      int res = AddEdges(isolate, direction, transaction, transactionCollection, &resolver, edgeIndex, result, args[0]);

      if (res != TRI_ERROR_NO_ERROR && res != TRI_ERROR_ARANGO_COLLECTION_NOT_FOUND) {
        // do not ignore error
        TRI_V8_THROW_EXCEPTION(res);
      }
    }

    TRI_V8_RETURN(result);
  }
  catch (triagens::arango::Exception const& ex) {
    TRI_V8_THROW_EXCEPTION(ex.code());
  }
  catch (...) {
    TRI_V8_THROW_EXCEPTION(TRI_ERROR_INTERNAL);
  }

  TRI_ASSERT(false);
}

////////////////////////////////////////////////////////////////////////////////
/// @brief selects connected edges
////////////////////////////////////////////////////////////////////////////////

static void JS_MvccEdges (const v8::FunctionCallbackInfo<v8::Value>& args) {
  MvccEdgesQuery(TRI_EDGE_ANY, args);
}

////////////////////////////////////////////////////////////////////////////////
/// @brief selects connected edges
////////////////////////////////////////////////////////////////////////////////

static void JS_MvccInEdges (const v8::FunctionCallbackInfo<v8::Value>& args) {
  MvccEdgesQuery(TRI_EDGE_IN, args);
}

////////////////////////////////////////////////////////////////////////////////
/// @brief selects connected edges
////////////////////////////////////////////////////////////////////////////////

static void JS_MvccOutEdges (const v8::FunctionCallbackInfo<v8::Value>& args) {
  MvccEdgesQuery(TRI_EDGE_OUT, args);
}

// -----------------------------------------------------------------------------
// --SECTION--                                                  temporal queries
// -----------------------------------------------------------------------------

////////////////////////////////////////////////////////////////////////////////
/// @brief helper function for MvccFirst and MvccLast
////////////////////////////////////////////////////////////////////////////////
  
static void MvccTemporalQuery (const v8::FunctionCallbackInfo<v8::Value>& args,
                               bool reverse) {
  v8::Isolate* isolate = args.GetIsolate();
  v8::HandleScope scope(isolate);
  
  triagens::mvcc::SearchOptions searchOptions;
  searchOptions.skip    = 0;
  searchOptions.limit   = 1;
  searchOptions.reverse = reverse;
  
  bool returnArray = false;

  // if argument is supplied, we return an array - otherwise we simply return the first doc
  if (args.Length() == 1) {
    if (! args[0]->IsUndefined()) {
      searchOptions.limit = TRI_ObjectToInt64(args[0]);
      returnArray = true;
    }
  }

  if (searchOptions.limit < 1) {
    TRI_V8_THROW_EXCEPTION_PARAMETER("invalid value for <count>");
  }
  

  auto const* collection = TRI_UnwrapClass<TRI_vocbase_col_t>(args.Holder(), TRI_GetVocBaseColType());

  if (collection == nullptr) {
    TRI_V8_THROW_EXCEPTION_INTERNAL("cannot extract collection");
  }
  
  // need a fake old transaction in order to not throw - can be removed later       
  TransactionBase oldTrx(true);
  CollectionNameResolver resolver(collection->_vocbase); // TODO
  
  triagens::mvcc::OperationOptions options;
  options.searchOptions = &searchOptions;

  try {
    triagens::mvcc::TransactionScope transactionScope(collection->_vocbase, triagens::mvcc::TransactionScope::NoCollections());

    auto* transaction = transactionScope.transaction();
    auto* transactionCollection = transaction->collection(collection->_cid);

    std::vector<TRI_doc_mptr_t const*> foundDocuments;
    auto searchResult = triagens::mvcc::CollectionOperations::ReadAllDocuments(&transactionScope, transactionCollection, foundDocuments, options);
 
    if (searchResult.code != TRI_ERROR_NO_ERROR) {
      THROW_ARANGO_EXCEPTION(searchResult.code);
    }
  
    if (returnArray) {
      size_t const n = foundDocuments.size();
      auto result = v8::Array::New(isolate, static_cast<int>(n));

      for (size_t i = 0; i < n; ++i) {
        v8::Handle<v8::Value> document = TRI_WrapShapedJson(isolate, &resolver, transactionCollection, foundDocuments[i]->getDataPtr());

        if (document.IsEmpty()) {
          TRI_V8_THROW_EXCEPTION_MEMORY();
        }
        result->Set(static_cast<uint32_t>(i), document);
      }

      TRI_V8_RETURN(result);
    }
      
    if (foundDocuments.empty()) {
      TRI_V8_RETURN_NULL();
    }
      
    v8::Handle<v8::Value> result = TRI_WrapShapedJson(isolate, &resolver, transactionCollection, foundDocuments[0]->getDataPtr());
    
    if (result.IsEmpty()) {
      TRI_V8_THROW_EXCEPTION_MEMORY();
    }
     
    TRI_V8_RETURN(result);
  }
  catch (triagens::arango::Exception const& ex) {
    TRI_V8_THROW_EXCEPTION_MESSAGE(ex.code(), ex.what());
  }
  catch (...) {
    TRI_V8_THROW_EXCEPTION(TRI_ERROR_INTERNAL);
  }
 
  // unreachable
  TRI_ASSERT(false);
}

////////////////////////////////////////////////////////////////////////////////
/// @brief selects the n first documents in the collection
////////////////////////////////////////////////////////////////////////////////

static void JS_MvccFirst (const v8::FunctionCallbackInfo<v8::Value>& args) {
  v8::Isolate* isolate = args.GetIsolate();
  v8::HandleScope scope(isolate);

  if (args.Length() > 1) {
    TRI_V8_THROW_EXCEPTION_USAGE("mvccFirst(<count>)");
  }

  MvccTemporalQuery(args, false);
}

////////////////////////////////////////////////////////////////////////////////
/// @brief selects the n last documents in the collection
////////////////////////////////////////////////////////////////////////////////

static void JS_MvccLast (const v8::FunctionCallbackInfo<v8::Value>& args) {
  v8::Isolate* isolate = args.GetIsolate();
  v8::HandleScope scope(isolate);

  if (args.Length() > 1) {
    TRI_V8_THROW_EXCEPTION_USAGE("mvccLast(<count>)");
  }

  MvccTemporalQuery(args, true);
}

// -----------------------------------------------------------------------------
// --SECTION--                                                       geo queries
// -----------------------------------------------------------------------------

////////////////////////////////////////////////////////////////////////////////
/// @brief selects documents around a reference coordinate
////////////////////////////////////////////////////////////////////////////////

static void JS_MvccNear (const v8::FunctionCallbackInfo<v8::Value>& args) {
  TransactionBase transBase(true);   // To protect against assertions, FIXME later
  v8::Isolate* isolate = args.GetIsolate();
  v8::HandleScope scope(isolate);
  
  auto const* collection = TRI_UnwrapClass<TRI_vocbase_col_t>(args.Holder(), TRI_GetVocBaseColType());

  if (collection == nullptr) {
    TRI_V8_THROW_EXCEPTION_INTERNAL("cannot extract collection");
  }
  
  TRI_THROW_SHARDING_COLLECTION_NOT_YET_IMPLEMENTED(collection);

  if (args.Length() != 4) {
    TRI_V8_THROW_EXCEPTION_USAGE("mvccNear(<index-handle>, <latitude>, <longitude>, <limit>)");
  }
    
  try {
    triagens::mvcc::TransactionScope transactionScope(collection->_vocbase, triagens::mvcc::TransactionScope::NoCollections());

    auto* transaction = transactionScope.transaction();
    auto* transactionCollection = transaction->collection(collection->_cid);
  
    // extract the index
    CollectionNameResolver resolver(collection->_vocbase); // TODO
    auto index = TRI_LookupMvccIndexByHandle(isolate, &resolver, collection, args[0]);
  
    if (index == nullptr ||
        (index->type() != TRI_IDX_TYPE_GEO1_INDEX &&
         index->type() != TRI_IDX_TYPE_GEO2_INDEX)) {
      TRI_V8_THROW_EXCEPTION(TRI_ERROR_ARANGO_NO_INDEX);
    }
  
    // extract latitude and longitude
    double latitude = TRI_ObjectToDouble(args[1]);
    double longitude = TRI_ObjectToDouble(args[2]);
  
    // extract the limit
    TRI_voc_ssize_t limit = (TRI_voc_ssize_t) TRI_ObjectToDouble(args[3]);
    if (limit <= 0) {
      limit = 100;
    }
  
    std::unique_ptr<std::vector<std::pair<TRI_doc_mptr_t*, double>>> indexResult(static_cast<triagens::mvcc::GeoIndex2*>(index)->near(transaction, latitude, longitude, limit));
    
    // setup result
    v8::Handle<v8::Object> result = v8::Object::New(isolate);
    v8::Handle<v8::Array> documents = v8::Array::New(isolate, indexResult->size());
    v8::Handle<v8::Array> distances = v8::Array::New(isolate, indexResult->size());
 
    uint32_t i = 0;
    for (auto const& it : *indexResult) {
      v8::Handle<v8::Value> document = TRI_WrapShapedJson(isolate, &resolver, transactionCollection, it.first->getDataPtr());

      if (document.IsEmpty()) {
        TRI_V8_THROW_EXCEPTION(TRI_ERROR_OUT_OF_MEMORY);
      }

      documents->Set(i, document);
      distances->Set(i, v8::Number::New(isolate, it.second));
      ++i; 
    }

    result->Set(TRI_V8_ASCII_STRING("documents"), documents);
    result->Set(TRI_V8_ASCII_STRING("distances"), distances);
    TRI_V8_RETURN(result);
  }
  catch (triagens::arango::Exception const& ex) {
    TRI_V8_THROW_EXCEPTION(ex.code());
  }
  catch (...) {
    TRI_V8_THROW_EXCEPTION(TRI_ERROR_INTERNAL);
  }

  TRI_ASSERT(false);
}

////////////////////////////////////////////////////////////////////////////////
/// @brief selects the documents within a radius
////////////////////////////////////////////////////////////////////////////////

static void JS_MvccWithin (const v8::FunctionCallbackInfo<v8::Value>& args) {
  TransactionBase transBase(true);   // To protect against assertions, FIXME later
  v8::Isolate* isolate = args.GetIsolate();
  v8::HandleScope scope(isolate);
  
  auto const* collection = TRI_UnwrapClass<TRI_vocbase_col_t>(args.Holder(), TRI_GetVocBaseColType());

  if (collection == nullptr) {
    TRI_V8_THROW_EXCEPTION_INTERNAL("cannot extract collection");
  }
  
  TRI_THROW_SHARDING_COLLECTION_NOT_YET_IMPLEMENTED(collection);

  if (args.Length() != 4) {
    TRI_V8_THROW_EXCEPTION_USAGE("mvccWithin(<index-handle>, <latitude>, <longitude>, <radius>)");
  }
    
  try {
    triagens::mvcc::TransactionScope transactionScope(collection->_vocbase, triagens::mvcc::TransactionScope::NoCollections());

    auto* transaction = transactionScope.transaction();
    auto* transactionCollection = transaction->collection(collection->_cid);
  
    // extract the index
    CollectionNameResolver resolver(collection->_vocbase); // TODO
    auto index = TRI_LookupMvccIndexByHandle(isolate, &resolver, collection, args[0]);
  
    if (index == nullptr ||
        (index->type() != TRI_IDX_TYPE_GEO1_INDEX &&
         index->type() != TRI_IDX_TYPE_GEO2_INDEX)) {
      TRI_V8_THROW_EXCEPTION(TRI_ERROR_ARANGO_NO_INDEX);
    }
  
    // extract latitude and longitude
    double latitude = TRI_ObjectToDouble(args[1]);
    double longitude = TRI_ObjectToDouble(args[2]);
    double radius = TRI_ObjectToDouble(args[3]);
  
    std::unique_ptr<std::vector<std::pair<TRI_doc_mptr_t*, double>>> indexResult(static_cast<triagens::mvcc::GeoIndex2*>(index)->within(transaction, latitude, longitude, radius));
    
    // setup result
    v8::Handle<v8::Object> result = v8::Object::New(isolate);
    v8::Handle<v8::Array> documents = v8::Array::New(isolate, indexResult->size());
    v8::Handle<v8::Array> distances = v8::Array::New(isolate, indexResult->size());
 
    uint32_t i = 0;
    for (auto const& it : *indexResult) {
      v8::Handle<v8::Value> document = TRI_WrapShapedJson(isolate, &resolver, transactionCollection, it.first->getDataPtr());

      if (document.IsEmpty()) {
        TRI_V8_THROW_EXCEPTION(TRI_ERROR_OUT_OF_MEMORY);
      }

      documents->Set(i, document);
      distances->Set(i, v8::Number::New(isolate, it.second));
      ++i; 
    }

    result->Set(TRI_V8_ASCII_STRING("documents"), documents);
    result->Set(TRI_V8_ASCII_STRING("distances"), distances);
    TRI_V8_RETURN(result);
  }
  catch (triagens::arango::Exception const& ex) {
    TRI_V8_THROW_EXCEPTION(ex.code());
  }
  catch (...) {
    TRI_V8_THROW_EXCEPTION(TRI_ERROR_INTERNAL);
  }

  TRI_ASSERT(false);
}


// -----------------------------------------------------------------------------
// --SECTION--                                                   private defines
// -----------------------------------------------------------------------------

////////////////////////////////////////////////////////////////////////////////
/// @brief shortcut to wrap a shaped-json object in a read-only transaction
////////////////////////////////////////////////////////////////////////////////

#define WRAP_SHAPED_JSON(...) TRI_WrapShapedJson<SingleCollectionReadOnlyTransaction>(isolate, __VA_ARGS__)

// -----------------------------------------------------------------------------
// --SECTION--                                                  HELPER FUNCTIONS
// -----------------------------------------------------------------------------

////////////////////////////////////////////////////////////////////////////////
/// @brief return an empty result set
////////////////////////////////////////////////////////////////////////////////

static v8::Handle<v8::Value> EmptyResult (v8::Isolate* isolate) {
  v8::EscapableHandleScope scope(isolate);

  v8::Handle<v8::Object> result = v8::Object::New(isolate);
  result->Set(TRI_V8_ASCII_STRING("documents"), v8::Array::New(isolate));
  result->Set(TRI_V8_ASCII_STRING("total"),     v8::Number::New(isolate, 0));
  result->Set(TRI_V8_ASCII_STRING("count"),     v8::Number::New(isolate, 0));

  return scope.Escape<v8::Value>(result);
}

// -----------------------------------------------------------------------------
// --SECTION--                                                     private types
// -----------------------------------------------------------------------------

////////////////////////////////////////////////////////////////////////////////
/// @brief geo coordinate container, also containing the distance
////////////////////////////////////////////////////////////////////////////////

typedef struct {
  double _distance;
  void const* _data;
}
geo_coordinate_distance_t;

////////////////////////////////////////////////////////////////////////////////
/// @brief query types
////////////////////////////////////////////////////////////////////////////////

typedef enum {
  QUERY_EXAMPLE,
  QUERY_CONDITION
}
query_t;

// -----------------------------------------------------------------------------
// --SECTION--                                                 private functions
// -----------------------------------------------------------------------------

////////////////////////////////////////////////////////////////////////////////
/// @brief cleans up the example object
////////////////////////////////////////////////////////////////////////////////

static void CleanupExampleObject (TRI_memory_zone_t* zone,
                                  size_t n,
                                  TRI_shape_pid_t* pids,
                                  TRI_shaped_json_t** values) {

  // clean shaped json objects
  for (size_t j = 0;  j < n;  ++j) {
    if (values[j] != 0) {
      TRI_FreeShapedJson(zone, values[j]);
    }
  }

  TRI_Free(TRI_UNKNOWN_MEM_ZONE, values);

  if (pids != 0) {
    TRI_Free(TRI_UNKNOWN_MEM_ZONE, pids);
  }
}

////////////////////////////////////////////////////////////////////////////////
/// @brief sets up the example object
////////////////////////////////////////////////////////////////////////////////

static int SetupExampleObject (v8::Handle<v8::Object> const example,
                               TRI_shaper_t* shaper,
                               size_t& n,
                               TRI_shape_pid_t*& pids,
                               TRI_shaped_json_t**& values,
                               const v8::FunctionCallbackInfo<v8::Value>& args) {
  v8::Isolate* isolate = args.GetIsolate();

  // get own properties of example
  v8::Handle<v8::Array> names = example->GetOwnPropertyNames();
  n = names->Length();

  // setup storage
  pids = (TRI_shape_pid_t*) TRI_Allocate(TRI_UNKNOWN_MEM_ZONE, n * sizeof(TRI_shape_pid_t), false);

  if (pids == 0) {
    // out of memory
    TRI_V8_SET_EXCEPTION_MEMORY();
    return TRI_ERROR_OUT_OF_MEMORY;
  }

  values = (TRI_shaped_json_t**) TRI_Allocate(TRI_UNKNOWN_MEM_ZONE,
                                              n * sizeof(TRI_shaped_json_t*), false);

  if (values == 0) {
    // out of memory
    TRI_Free(TRI_UNKNOWN_MEM_ZONE, pids);
    pids = 0;
    TRI_V8_SET_EXCEPTION_MEMORY();
    return TRI_ERROR_OUT_OF_MEMORY;
  }

  // convert
  for (size_t i = 0;  i < n;  ++i) {
    v8::Handle<v8::Value> key = names->Get((uint32_t) i);
    v8::Handle<v8::Value> val = example->Get(key);

    // property initialise the memory
    values[i] = 0;

    TRI_Utf8ValueNFC keyStr(TRI_UNKNOWN_MEM_ZONE, key);

    if (*keyStr != 0) {
      pids[i] = shaper->lookupAttributePathByName(shaper, *keyStr);

      if (pids[i] == 0) {
        // no attribute path found. this means the result will be empty
        CleanupExampleObject(shaper->_memoryZone, i, pids, values);
        return TRI_RESULT_ELEMENT_NOT_FOUND;
      }

      values[i] = TRI_ShapedJsonV8Object(isolate, val, shaper, false);

      if (values[i] == 0) {
        CleanupExampleObject(shaper->_memoryZone, i, pids, values);
        return TRI_RESULT_ELEMENT_NOT_FOUND;
      }
    }
    else {
      CleanupExampleObject(shaper->_memoryZone, i, pids, values);
      TRI_V8_SET_EXCEPTION_MESSAGE(TRI_ERROR_BAD_PARAMETER,
                                   "cannot convert attribute path to UTF8");
      return TRI_ERROR_BAD_PARAMETER;
    }
  }

  return TRI_ERROR_NO_ERROR;
}

////////////////////////////////////////////////////////////////////////////////
/// @brief sets up the skiplist operator for a skiplist condition query
////////////////////////////////////////////////////////////////////////////////

static TRI_index_operator_t* SetupConditionsSkiplist (v8::Isolate* isolate,
                                                      TRI_index_t* idx,
                                                      TRI_shaper_t* shaper,
                                                      v8::Handle<v8::Object> conditions) {
  TRI_index_operator_t* lastOperator = nullptr;
  size_t numEq = 0;
  size_t lastNonEq = 0;

  TRI_json_t* parameters = TRI_CreateArrayJson(TRI_UNKNOWN_MEM_ZONE);

  if (parameters == nullptr) {
    return nullptr;
  }

  // iterate over all index fields
  for (size_t i = 1; i <= idx->_fields._length; ++i) {
    v8::Handle<v8::String> key = TRI_V8_STRING(idx->_fields._buffer[i - 1]);

    if (! conditions->HasOwnProperty(key)) {
      break;
    }
    v8::Handle<v8::Value> fieldConditions = conditions->Get(key);

    if (! fieldConditions->IsArray()) {
      // wrong data type for field conditions
      break;
    }

    // iterator over all conditions
    v8::Handle<v8::Array> values = v8::Handle<v8::Array>::Cast(fieldConditions);
    for (uint32_t j = 0; j < values->Length(); ++j) {
      v8::Handle<v8::Value> fieldCondition = values->Get(j);

      if (! fieldCondition->IsArray()) {
        // wrong data type for single condition
        goto MEM_ERROR;
      }

      v8::Handle<v8::Array> condition = v8::Handle<v8::Array>::Cast(fieldCondition);

      if (condition->Length() != 2) {
        // wrong number of values in single condition
        goto MEM_ERROR;
      }

      v8::Handle<v8::Value> op = condition->Get(0);
      v8::Handle<v8::Value> value = condition->Get(1);

      if (! op->IsString() && ! op->IsStringObject()) {
        // wrong operator type
        goto MEM_ERROR;
      }

      TRI_json_t* json = TRI_ObjectToJson(isolate, value);

      if (json == nullptr) {
        goto MEM_ERROR;
      }

      std::string&& opValue = TRI_ObjectToString(op);
      if (opValue == "==") {
        // equality comparison

        if (lastNonEq > 0) {
          TRI_FreeJson(TRI_UNKNOWN_MEM_ZONE, json);
          goto MEM_ERROR;
        }

        TRI_PushBack3ArrayJson(TRI_UNKNOWN_MEM_ZONE, parameters, json);
        // creation of equality operator is deferred until it is finally needed
        ++numEq;
        break;
      }
      else {
        if (lastNonEq > 0 && lastNonEq != i) {
          // if we already had a range condition and a previous field, we cannot continue
          // because the skiplist interface does not support such queries
          TRI_FreeJson(TRI_UNKNOWN_MEM_ZONE, json);
          goto MEM_ERROR;
        }

        TRI_index_operator_type_e opType;
        if (opValue == ">") {
          opType = TRI_GT_INDEX_OPERATOR;
        }
        else if (opValue == ">=") {
          opType = TRI_GE_INDEX_OPERATOR;
        }
        else if (opValue == "<") {
          opType = TRI_LT_INDEX_OPERATOR;
        }
        else if (opValue == "<=") {
          opType = TRI_LE_INDEX_OPERATOR;
        }
        else {
          // wrong operator type
          TRI_FreeJson(TRI_UNKNOWN_MEM_ZONE, json);
          goto MEM_ERROR;
        }

        lastNonEq = i;

        TRI_json_t* cloned = TRI_CopyJson(TRI_UNKNOWN_MEM_ZONE, parameters);

        if (cloned == nullptr) {
          TRI_FreeJson(TRI_UNKNOWN_MEM_ZONE, json);
          goto MEM_ERROR;
        }

        TRI_PushBack3ArrayJson(TRI_UNKNOWN_MEM_ZONE, cloned, json);

        if (numEq) {
          // create equality operator if one is in queue
          TRI_json_t* clonedParams = TRI_CopyJson(TRI_UNKNOWN_MEM_ZONE, parameters);

          if (clonedParams == nullptr) {
            TRI_FreeJson(TRI_UNKNOWN_MEM_ZONE, cloned);
            goto MEM_ERROR;
          }

          lastOperator = TRI_CreateIndexOperator(TRI_EQ_INDEX_OPERATOR, 
                                                 nullptr,
                                                 nullptr, 
                                                 clonedParams, 
                                                 shaper, 
                                                 clonedParams->_value._objects._length); 
          numEq = 0;
        }

        TRI_index_operator_t* current;

        // create the operator for the current condition
        current = TRI_CreateIndexOperator(opType,
                                          nullptr,
                                          nullptr, 
                                          cloned, 
                                          shaper, 
                                          cloned->_value._objects._length); 

        if (current == nullptr) {
          TRI_FreeJson(TRI_UNKNOWN_MEM_ZONE, cloned);
          goto MEM_ERROR;
        }

        if (lastOperator == nullptr) {
          lastOperator = current;
        }
        else {
          // merge the current operator with previous operators using logical AND
          TRI_index_operator_t* newOperator = TRI_CreateIndexOperator(TRI_AND_INDEX_OPERATOR, 
                                                                      lastOperator, 
                                                                      current, 
                                                                      nullptr, 
                                                                      shaper, 
                                                                      2);

          if (newOperator == nullptr) {
            TRI_FreeIndexOperator(current);
            goto MEM_ERROR;
          }
          else {
            lastOperator = newOperator;
          }
        }
      }
    }

  }

  if (numEq) {
    // create equality operator if one is in queue
    TRI_ASSERT(lastOperator == nullptr);
    TRI_ASSERT(lastNonEq == 0);

    TRI_json_t* clonedParams = TRI_CopyJson(TRI_UNKNOWN_MEM_ZONE, parameters);

    if (clonedParams == nullptr) {
      goto MEM_ERROR;
    }

    lastOperator = TRI_CreateIndexOperator(TRI_EQ_INDEX_OPERATOR, 
                                           nullptr,
                                           nullptr,
                                           clonedParams, 
                                           shaper, 
                                           clonedParams->_value._objects._length); 
  }

  TRI_FreeJson(TRI_UNKNOWN_MEM_ZONE, parameters);

  return lastOperator;

MEM_ERROR:
  TRI_FreeJson(TRI_UNKNOWN_MEM_ZONE, parameters);

  if (lastOperator != nullptr) {
    TRI_FreeIndexOperator(lastOperator);
  }

  return nullptr;
}

////////////////////////////////////////////////////////////////////////////////
/// @brief sets up the skiplist operator for a skiplist example query
///
/// this will set up a JSON container with the example values as a list
/// at the end, one skiplist equality operator is created for the entire list
////////////////////////////////////////////////////////////////////////////////

static TRI_index_operator_t* SetupExampleSkiplist (v8::Isolate* isolate,
                                                   TRI_index_t* idx,
                                                   TRI_shaper_t* shaper,
                                                   v8::Handle<v8::Object> example) {
  TRI_json_t* parameters = TRI_CreateArrayJson(TRI_UNKNOWN_MEM_ZONE);

  if (parameters == nullptr) {
    return nullptr;
  }

  for (size_t i = 0; i < idx->_fields._length; ++i) {
    v8::Handle<v8::String> key = TRI_V8_STRING(idx->_fields._buffer[i]);

    if (! example->HasOwnProperty(key)) {
      break;
    }

    v8::Handle<v8::Value> value = example->Get(key);

    TRI_json_t* json = TRI_ObjectToJson(isolate, value);

    if (json == nullptr) {
      TRI_FreeJson(TRI_UNKNOWN_MEM_ZONE, parameters);

      return nullptr;
    }

    TRI_PushBack3ArrayJson(TRI_UNKNOWN_MEM_ZONE, parameters, json);
  }

  if (parameters->_value._objects._length > 0) {
    // example means equality comparisons only
    return TRI_CreateIndexOperator(TRI_EQ_INDEX_OPERATOR, 
                                   nullptr, 
                                   nullptr,
                                   parameters, 
                                   shaper, 
                                   parameters->_value._objects._length);
  }

  TRI_FreeJson(TRI_UNKNOWN_MEM_ZONE, parameters);
  return nullptr;
}

////////////////////////////////////////////////////////////////////////////////
/// @brief destroys the example object for a hash index
////////////////////////////////////////////////////////////////////////////////

static void DestroySearchValue (TRI_memory_zone_t* zone,
                                TRI_index_search_value_t& value) {
  size_t n = value._length;

  for (size_t j = 0;  j < n;  ++j) {
    TRI_DestroyShapedJson(zone, &value._values[j]);
  }

  TRI_Free(TRI_CORE_MEM_ZONE, value._values);
}

////////////////////////////////////////////////////////////////////////////////
/// @brief sets up the example object for a hash index
////////////////////////////////////////////////////////////////////////////////

static int SetupSearchValue (TRI_vector_t const* paths,
                             v8::Handle<v8::Object> example,
                             TRI_shaper_t* shaper,
                             TRI_index_search_value_t& result,
                             const v8::FunctionCallbackInfo<v8::Value>& args) {
  v8::Isolate* isolate = args.GetIsolate();
  size_t n;

  // extract attribute paths
  n = paths->_length;

  // setup storage
  result._length = n;
  result._values = (TRI_shaped_json_t*) TRI_Allocate(TRI_CORE_MEM_ZONE,
                                                     n * sizeof(TRI_shaped_json_t),
                                                     true);

  // convert
  for (size_t i = 0;  i < n;  ++i) {
    TRI_shape_pid_t pid = * (TRI_shape_pid_t*) TRI_AtVector(paths, i);

    TRI_ASSERT(pid != 0);
    char const* name = TRI_AttributeNameShapePid(shaper, pid);

    if (name == NULL) {
      DestroySearchValue(shaper->_memoryZone, result);
      TRI_V8_SET_EXCEPTION_MESSAGE(TRI_ERROR_INTERNAL,
                                   "shaper failed");
      return TRI_ERROR_BAD_PARAMETER;
    }

    v8::Handle<v8::String> key = TRI_V8_STRING(name);
    int res;

    if (example->HasOwnProperty(key)) {
      v8::Handle<v8::Value> val = example->Get(key);

      res = TRI_FillShapedJsonV8Object(isolate, val, &result._values[i], shaper, false);
    }
    else {
      res = TRI_FillShapedJsonV8Object(isolate, v8::Null(isolate), &result._values[i], shaper, false);
    }

    if (res != TRI_ERROR_NO_ERROR) {
      DestroySearchValue(shaper->_memoryZone, result);

      if (res != TRI_RESULT_ELEMENT_NOT_FOUND) {
        TRI_V8_SET_EXCEPTION_MESSAGE(res, "cannot convert value to JSON");
        return res;
      }
      return res;
    }
  }

  return TRI_ERROR_NO_ERROR;
}

////////////////////////////////////////////////////////////////////////////////
/// @brief execute a skiplist query (by condition or by example)
////////////////////////////////////////////////////////////////////////////////

static void ExecuteSkiplistQuery (const v8::FunctionCallbackInfo<v8::Value>& args,
                                                   std::string const& signature,
                                                   query_t type) {
  v8::Isolate* isolate = args.GetIsolate();
  v8::HandleScope scope(isolate);

  // expecting index, example, skip, and limit
  if (args.Length() < 2) {
    TRI_V8_THROW_EXCEPTION_USAGE(signature.c_str());
  }

  if (! args[1]->IsObject()) {
    std::string msg;

    if (type == QUERY_EXAMPLE) {
      msg = "<example> must be an object";
    }
    else {
      msg = "<conditions> must be an object";
    }

    TRI_V8_THROW_TYPE_ERROR(msg.c_str());
  }

  bool reverse = false;
  if (args.Length() > 4) {
    reverse = TRI_ObjectToBoolean(args[4]);
  }

  TRI_vocbase_col_t const* col;
  col = TRI_UnwrapClass<TRI_vocbase_col_t>(args.Holder(), TRI_GetVocBaseColType());

  if (col == nullptr) {
    TRI_V8_THROW_EXCEPTION_INTERNAL("cannot extract collection");
  }

  TRI_THROW_SHARDING_COLLECTION_NOT_YET_IMPLEMENTED(col);

  SingleCollectionReadOnlyTransaction trx(new V8TransactionContext(true), col->_vocbase, col->_cid);

  int res = trx.begin();

  if (res != TRI_ERROR_NO_ERROR) {
    TRI_V8_THROW_EXCEPTION(res);
  }

  v8::Handle<v8::Object> err;

  TRI_document_collection_t* document = trx.documentCollection();
  TRI_shaper_t* shaper = document->getShaper();  // PROTECTED by trx here

  // extract skip and limit
  TRI_voc_ssize_t skip;
  TRI_voc_size_t limit;

  ExtractSkipAndLimit(args, 2, skip, limit);

  // setup result
  v8::Handle<v8::Object> result = v8::Object::New(isolate);

  v8::Handle<v8::Array> documents = v8::Array::New(isolate);
  result->Set(TRI_V8_ASCII_STRING("documents"), documents);

  // .............................................................................
  // inside a read transaction
  // .............................................................................

  trx.lockRead();


  // extract the index
  TRI_index_t* idx = TRI_LookupIndexByHandle(isolate, trx.resolver(), col, args[0], false);

  if (idx == nullptr) {
    return;
  }

  if (idx->_type != TRI_IDX_TYPE_SKIPLIST_INDEX) {
    TRI_V8_THROW_EXCEPTION(TRI_ERROR_ARANGO_NO_INDEX);
  }

  TRI_index_operator_t* skiplistOperator;
  v8::Handle<v8::Object> values = args[1]->ToObject();

  if (type == QUERY_EXAMPLE) {
    skiplistOperator = SetupExampleSkiplist(isolate, idx, shaper, values);
  }
  else {
    skiplistOperator = SetupConditionsSkiplist(isolate, idx, shaper, values);
  }

  if (skiplistOperator == nullptr) {
    TRI_V8_THROW_EXCEPTION(TRI_ERROR_BAD_PARAMETER);
  }

  TRI_skiplist_iterator_t* skiplistIterator = TRI_LookupSkiplistIndex(idx, skiplistOperator, reverse);
  TRI_FreeIndexOperator(skiplistOperator);

  if (skiplistIterator == nullptr) {
    int res = TRI_errno();
    if (res == TRI_RESULT_ELEMENT_NOT_FOUND) {
      TRI_V8_RETURN(EmptyResult(isolate));
    }

    TRI_V8_THROW_EXCEPTION(TRI_ERROR_ARANGO_NO_INDEX);
  }

  TRI_voc_ssize_t total = 0;
  TRI_voc_size_t count = 0;
  bool error = false;

  if (trx.orderBarrier(trx.trxCollection()) == nullptr) {
    TRI_FreeSkiplistIterator(skiplistIterator);
    TRI_V8_THROW_EXCEPTION_MEMORY();
  }

  while (limit > 0) {
    TRI_skiplist_index_element_t* indexElement = skiplistIterator->next(skiplistIterator);

    if (indexElement == nullptr) {
      break;
    }

    ++total;

    if (total > skip && count < limit) {
      v8::Handle<v8::Value> doc = WRAP_SHAPED_JSON(trx,
                                                   col->_cid,
                                                   ((TRI_doc_mptr_t const*) indexElement->_document)->getDataPtr());

      if (doc.IsEmpty()) {
        error = true;
        break;
      }
      else {
        documents->Set((uint32_t) count, doc);

        if (++count >= limit) {
          break;
        }
      }

    }
  }

  trx.finish(res);

  // .............................................................................
  // outside a write transaction
  // .............................................................................

  // free data allocated by skiplist index result
  TRI_FreeSkiplistIterator(skiplistIterator);

  result->Set(TRI_V8_ASCII_STRING("total"), v8::Number::New(isolate, (double) total));
  result->Set(TRI_V8_ASCII_STRING("count"), v8::Number::New(isolate, count));

  if (error) {
    TRI_V8_THROW_EXCEPTION_MEMORY();
  }

  return TRI_V8_RETURN(result);
}

////////////////////////////////////////////////////////////////////////////////
/// @brief creates a geo result
////////////////////////////////////////////////////////////////////////////////

static int StoreGeoResult (v8::Isolate* isolate,
                           SingleCollectionReadOnlyTransaction& trx,
                           TRI_vocbase_col_t const* collection,
                           GeoCoordinates* cors,
                           v8::Handle<v8::Array>& documents,
                           v8::Handle<v8::Array>& distances) {
  GeoCoordinate* end;
  GeoCoordinate* ptr;
  double* dtr;
  geo_coordinate_distance_t* gtr;
  geo_coordinate_distance_t* tmp;
  uint32_t i;

  if (trx.orderBarrier(trx.trxCollection()) == nullptr) {
    GeoIndex_CoordinatesFree(cors);
    return TRI_ERROR_OUT_OF_MEMORY;
  }

  // sort the result
  size_t n = cors->length;

  if (n == 0) {
    GeoIndex_CoordinatesFree(cors);
    return TRI_ERROR_NO_ERROR;
  }

  gtr = (tmp = (geo_coordinate_distance_t*) TRI_Allocate(TRI_UNKNOWN_MEM_ZONE, sizeof(geo_coordinate_distance_t) * n, false));

  if (gtr == nullptr) {
    GeoIndex_CoordinatesFree(cors);

    return TRI_ERROR_OUT_OF_MEMORY;
  }

  geo_coordinate_distance_t* gnd = tmp + n;

  ptr = cors->coordinates;
  end = cors->coordinates + n;

  dtr = cors->distances;

  for (;  ptr < end;  ++ptr, ++dtr, ++gtr) {
    gtr->_distance = *dtr;
    gtr->_data = ptr->data;
  }

  GeoIndex_CoordinatesFree(cors);

  // sort result by distance
  auto compareSort = [] (geo_coordinate_distance_t const& left, geo_coordinate_distance_t const& right) {
    return left._distance < right._distance;
  };
  std::sort(tmp, gnd, compareSort);

  // copy the documents
  bool error = false;
  for (gtr = tmp, i = 0;  gtr < gnd;  ++gtr, ++i) {
    v8::Handle<v8::Value> doc = WRAP_SHAPED_JSON(trx, collection->_cid, ((TRI_doc_mptr_t const*) gtr->_data)->getDataPtr());

    if (doc.IsEmpty()) {
      error = true;
      break;
    }

    documents->Set(i, doc);
    distances->Set(i, v8::Number::New(isolate, gtr->_distance));
  }

  TRI_Free(TRI_UNKNOWN_MEM_ZONE, tmp);

  if (error) {
    return TRI_ERROR_OUT_OF_MEMORY;
  }

  return TRI_ERROR_NO_ERROR;
}

// -----------------------------------------------------------------------------
// --SECTION--                                                   QUERY FUNCTIONS
// -----------------------------------------------------------------------------

// -----------------------------------------------------------------------------
// --SECTION--                                                 private functions
// -----------------------------------------------------------------------------

////////////////////////////////////////////////////////////////////////////////
/// @brief looks up edges for given direction
////////////////////////////////////////////////////////////////////////////////

static void EdgesQuery (TRI_edge_direction_e direction,
                                         const v8::FunctionCallbackInfo<v8::Value>& args) {
  v8::Isolate* isolate = args.GetIsolate();
  v8::HandleScope scope(isolate);

  TRI_vocbase_col_t const* col;
  col = TRI_UnwrapClass<TRI_vocbase_col_t>(args.Holder(), TRI_GetVocBaseColType());

  if (col == nullptr) {
    TRI_V8_THROW_EXCEPTION_INTERNAL("cannot extract collection");
  }

  TRI_THROW_SHARDING_COLLECTION_NOT_YET_IMPLEMENTED(col);

  if (col->_type != TRI_COL_TYPE_EDGE) {
    TRI_V8_THROW_EXCEPTION(TRI_ERROR_ARANGO_COLLECTION_TYPE_INVALID);
  }

  SingleCollectionReadOnlyTransaction trx(new V8TransactionContext(true), col->_vocbase, col->_cid);

  int res = trx.begin();

  if (res != TRI_ERROR_NO_ERROR) {
    TRI_V8_THROW_EXCEPTION(res);
  }

  TRI_document_collection_t* document = trx.documentCollection();

  // first and only argument schould be a list of document idenfifier
  if (args.Length() != 1) {
    switch (direction) {
      case TRI_EDGE_IN:
        TRI_V8_THROW_EXCEPTION_USAGE("inEdges(<vertices>)");

      case TRI_EDGE_OUT:
        TRI_V8_THROW_EXCEPTION_USAGE("outEdges(<vertices>)");

      case TRI_EDGE_ANY:
      default: {
        TRI_V8_THROW_EXCEPTION_USAGE("edges(<vertices>)");
      }
    }
  }

  // setup result
  v8::Handle<v8::Array> documents;

  // .............................................................................
  // inside a read transaction
  // .............................................................................

  trx.lockRead();

  bool error = false;

  if (trx.orderBarrier(trx.trxCollection()) == nullptr) {
    TRI_V8_THROW_EXCEPTION_MEMORY();
  }

  // argument is a list of vertices
  if (args[0]->IsArray()) {
    documents = v8::Array::New(isolate);
    v8::Handle<v8::Array> vertices = v8::Handle<v8::Array>::Cast(args[0]);
    uint32_t count = 0;
    const uint32_t len = vertices->Length();

    for (uint32_t i = 0;  i < len; ++i) {
      TRI_voc_cid_t cid;
      std::unique_ptr<char[]> key;

      res = TRI_ParseVertex(args, trx.resolver(), cid, key,
                            vertices->Get(i));

      if (res != TRI_ERROR_NO_ERROR) {
        // error is just ignored
        continue;
      }

      std::vector<TRI_doc_mptr_copy_t>&& edges = TRI_LookupEdgesDocumentCollection(document, direction, cid, key.get());

      for (size_t j = 0;  j < edges.size();  ++j) {
        v8::Handle<v8::Value> doc = WRAP_SHAPED_JSON(trx, col->_cid, edges[j].getDataPtr());

        if (doc.IsEmpty()) {
          // error
          error = true;
          break;
        }
        else {
          documents->Set(count, doc);
          ++count;
        }

      }

      if (error) {
        break;
      }
    }
    trx.finish(res);
  }

  // argument is a single vertex
  else {
    std::unique_ptr<char[]> key;
    TRI_voc_cid_t cid;

    res = TRI_ParseVertex(args, trx.resolver(), cid, key, args[0]);

    if (res != TRI_ERROR_NO_ERROR) {
      TRI_V8_THROW_EXCEPTION(res);
    }

    std::vector<TRI_doc_mptr_copy_t>&& edges = TRI_LookupEdgesDocumentCollection(document, direction, cid, key.get());

    trx.finish(res);

    uint32_t const n = static_cast<uint32_t>(edges.size());
    documents = v8::Array::New(isolate, static_cast<int>(n));
    for (size_t j = 0;  j < n;  ++j) {
      v8::Handle<v8::Value> doc = WRAP_SHAPED_JSON(trx, col->_cid, edges[j].getDataPtr());

      if (doc.IsEmpty()) {
        error = true;
        break;
      }
      else {
        documents->Set(static_cast<uint32_t>(j), doc);
      }
    }
  }

  // .............................................................................
  // outside a read transaction
  // .............................................................................

  if (error) {
    TRI_V8_THROW_EXCEPTION_MEMORY();
  }

  TRI_V8_RETURN(documents);
}

// -----------------------------------------------------------------------------
// --SECTION--                                              javascript functions
// -----------------------------------------------------------------------------

////////////////////////////////////////////////////////////////////////////////
/// @brief selects all documents from a collection
////////////////////////////////////////////////////////////////////////////////

static void JS_AllQuery (const v8::FunctionCallbackInfo<v8::Value>& args) {
  v8::Isolate* isolate = args.GetIsolate();
  v8::HandleScope scope(isolate);

  // expecting two arguments
  if (args.Length() != 2) {
    TRI_V8_THROW_EXCEPTION_USAGE("ALL(<skip>, <limit>)");
  }

  TRI_vocbase_col_t const* col;
  col = TRI_UnwrapClass<TRI_vocbase_col_t>(args.Holder(), TRI_GetVocBaseColType());

  if (col == nullptr) {
    TRI_V8_THROW_EXCEPTION_INTERNAL("cannot extract collection");
  }

  TRI_THROW_SHARDING_COLLECTION_NOT_YET_IMPLEMENTED(col);

  // extract skip and limit
  TRI_voc_ssize_t skip;
  TRI_voc_size_t limit;
  ExtractSkipAndLimit(args, 0, skip, limit);

  uint32_t total = 0;
  vector<TRI_doc_mptr_copy_t> docs;

  SingleCollectionReadOnlyTransaction trx(new V8TransactionContext(true), col->_vocbase, col->_cid);

  int res = trx.begin();

  if (res != TRI_ERROR_NO_ERROR) {
    TRI_V8_THROW_EXCEPTION(res);
  }
 
  res = trx.read(docs, skip, limit, &total);
  TRI_ASSERT(docs.empty() || trx.hasBarrier());
  
  res = trx.finish(res);

  if (res != TRI_ERROR_NO_ERROR) {
    TRI_V8_THROW_EXCEPTION(res);
  }

  size_t const n = docs.size();
  uint32_t count = 0;

  // setup result
  v8::Handle<v8::Object> result = v8::Object::New(isolate);
  v8::Handle<v8::Array> documents = v8::Array::New(isolate, (int) n);
  // reserve full capacity in one go
  result->Set(TRI_V8_ASCII_STRING("documents"), documents);

  for (size_t i = 0; i < n; ++i) {
    v8::Handle<v8::Value> doc = WRAP_SHAPED_JSON(trx, col->_cid, docs[i].getDataPtr());

    if (doc.IsEmpty()) {
      TRI_V8_THROW_EXCEPTION_MEMORY();
    }
    else {
      documents->Set(count++, doc);
    }
  }

  result->Set(TRI_V8_ASCII_STRING("total"), v8::Number::New(isolate, total));
  result->Set(TRI_V8_ASCII_STRING("count"), v8::Number::New(isolate, count));
 
  TRI_V8_RETURN(result);
}

////////////////////////////////////////////////////////////////////////////////
/// @brief selects a random document
///
/// @FUN{@FA{collection}.any()}
///
/// The @FN{any} method returns a random document from the collection.  It returns
/// @LIT{null} if the collection is empty.
///
/// @EXAMPLES
///
/// @code
/// arangod> db.example.any()
/// { "_id" : "example/222716379559", "_rev" : "222716379559", "Hello" : "World" }
/// @endcode
////////////////////////////////////////////////////////////////////////////////

static void JS_AnyQuery (const v8::FunctionCallbackInfo<v8::Value>& args) {
  v8::Isolate* isolate = args.GetIsolate();
  v8::HandleScope scope(isolate);

  TRI_vocbase_col_t const* col;
  col = TRI_UnwrapClass<TRI_vocbase_col_t>(args.Holder(), TRI_GetVocBaseColType());

  if (col == nullptr) {
    TRI_V8_THROW_EXCEPTION_INTERNAL("cannot extract collection");
  }

  TRI_doc_mptr_copy_t document;

  SingleCollectionReadOnlyTransaction trx(new V8TransactionContext(true), col->_vocbase, col->_cid);

  int res = trx.begin();

  if (res != TRI_ERROR_NO_ERROR) {
    TRI_V8_THROW_EXCEPTION(res);
  }

  res = trx.readRandom(&document);
  TRI_ASSERT(document.getDataPtr() == nullptr || trx.hasBarrier());

  res = trx.finish(res);

  if (res != TRI_ERROR_NO_ERROR) {
    TRI_V8_THROW_EXCEPTION(res);
  }

  if (document.getDataPtr() == nullptr) {  // PROTECTED by trx here
    TRI_V8_RETURN_NULL();
  }

  v8::Handle<v8::Value> doc = WRAP_SHAPED_JSON(trx, col->_cid, document.getDataPtr());

  if (doc.IsEmpty()) {
    TRI_V8_THROW_EXCEPTION_MEMORY();
  }

  TRI_V8_RETURN(doc);
}

////////////////////////////////////////////////////////////////////////////////
/// @brief selects documents by example (not using any index)
////////////////////////////////////////////////////////////////////////////////

static void JS_ByExampleQuery (const v8::FunctionCallbackInfo<v8::Value>& args) {
  v8::Isolate* isolate = args.GetIsolate();
  v8::HandleScope scope(isolate);

  // expecting example, skip, limit
  if (args.Length() < 1) {
    TRI_V8_THROW_EXCEPTION_USAGE("BY_EXAMPLE(<example>, <skip>, <limit>)");
  }

  // extract the example
  if (! args[0]->IsObject()) {
    TRI_V8_THROW_TYPE_ERROR("<example> must be an object");
  }


  TRI_vocbase_col_t const* col;
  col = TRI_UnwrapClass<TRI_vocbase_col_t>(args.Holder(), TRI_GetVocBaseColType());

  if (col == nullptr) {
    TRI_V8_THROW_EXCEPTION_INTERNAL("cannot extract collection");
  }

  TRI_THROW_SHARDING_COLLECTION_NOT_YET_IMPLEMENTED(col);

  SingleCollectionReadOnlyTransaction trx(new V8TransactionContext(true), col->_vocbase, col->_cid);

  int res = trx.begin();

  if (res != TRI_ERROR_NO_ERROR) {
    TRI_V8_THROW_EXCEPTION(res);
  }

  TRI_document_collection_t* document = trx.documentCollection();
  TRI_shaper_t* shaper = document->getShaper();  // PROTECTED by trx here

  v8::Handle<v8::Object> example = args[0]->ToObject();

  // extract skip and limit
  TRI_voc_ssize_t skip;
  TRI_voc_size_t limit;
  ExtractSkipAndLimit(args, 1, skip, limit);

  // extract sub-documents
  TRI_shape_pid_t* pids;
  TRI_shaped_json_t** values = 0;
  size_t n;

  if (trx.orderBarrier(trx.trxCollection()) == nullptr) {
    TRI_V8_THROW_EXCEPTION_MEMORY();
  }

  v8::Handle<v8::Object> err;
  res = SetupExampleObject(example, shaper, n, pids, values, args);

  if (res == TRI_RESULT_ELEMENT_NOT_FOUND) {
    // empty result
    TRI_V8_RETURN(EmptyResult(isolate));
  }

  if (res != TRI_ERROR_NO_ERROR) {
    return;
  }

  // setup result
  v8::Handle<v8::Object> result = v8::Object::New(isolate);
  v8::Handle<v8::Array> documents = v8::Array::New(isolate);
  result->Set(TRI_V8_ASCII_STRING("documents"), documents);

  // ...........................................................................
  // inside a read transaction
  // ...........................................................................

  vector<TRI_doc_mptr_copy_t> filtered;

  trx.lockRead();

  // find documents by example
  try {
    filtered = TRI_SelectByExample(trx.trxCollection(), n,  pids, values);
  }
  catch (std::exception&) {
    TRI_V8_THROW_EXCEPTION_MEMORY();
  }

  trx.finish(res);

  // ...........................................................................
  // outside a read transaction
  // ...........................................................................

  // convert to list of shaped jsons
  size_t total = filtered.size();
  size_t count = 0;
  bool error = false;

  if (0 < total) {
    size_t s;
    size_t e;

    CalculateSkipLimitSlice(filtered.size(), skip, limit, s, e);

    if (s < e) {
      for (size_t j = s; j < e; ++j) {
        v8::Handle<v8::Value> doc = WRAP_SHAPED_JSON(trx, col->_cid, filtered[j].getDataPtr());

        if (doc.IsEmpty()) {
          error = true;
          break;
        }
        else {
          documents->Set((uint32_t) count++, doc);
        }
      }
    }
  }


  result->Set(TRI_V8_ASCII_STRING("total"), v8::Integer::New(isolate, (int32_t) total));
  result->Set(TRI_V8_ASCII_STRING("count"), v8::Integer::New(isolate, (int32_t) count));

  CleanupExampleObject(shaper->_memoryZone, n, pids, values);

  if (error) {
    TRI_V8_THROW_EXCEPTION_MEMORY();
  }

  TRI_V8_RETURN(result);
}

////////////////////////////////////////////////////////////////////////////////
/// @brief selects documents by example using a hash index
///
/// It is the callers responsibility to acquire and free the required locks
////////////////////////////////////////////////////////////////////////////////

static void ByExampleHashIndexQuery (SingleCollectionReadOnlyTransaction& trx,
                                     TRI_vocbase_col_t const* collection,
                                     const v8::FunctionCallbackInfo<v8::Value>& args) {
  v8::Isolate* isolate = args.GetIsolate();
  v8::HandleScope scope(isolate);

  // expecting index, example, skip, and limit
  if (args.Length() < 2) {
    TRI_V8_THROW_EXCEPTION_USAGE("EXAMPLE_HASH(<index>, <example>, <skip>, <limit>)");
  }

  // extract the example
  if (! args[1]->IsObject()) {
    TRI_V8_THROW_TYPE_ERROR("<example> must be an object");
  }

  v8::Handle<v8::Object> example = args[1]->ToObject();

  if (trx.orderBarrier(trx.trxCollection()) == nullptr) {
    TRI_V8_THROW_EXCEPTION_MEMORY();
  }

  // extract skip and limit
  TRI_voc_ssize_t skip;
  TRI_voc_size_t limit;

  ExtractSkipAndLimit(args, 2, skip, limit);

  // setup result
  v8::Handle<v8::Object> result = v8::Object::New(isolate);

  v8::Handle<v8::Array> documents = v8::Array::New(isolate);
  result->Set(TRI_V8_ASCII_STRING("documents"), documents);

  // extract the index
  TRI_index_t* idx = TRI_LookupIndexByHandle(isolate, trx.resolver(), collection, args[0], false);

  if (idx == nullptr) {
    return;
  }

  if (idx->_type != TRI_IDX_TYPE_HASH_INDEX) {
    TRI_V8_THROW_EXCEPTION(TRI_ERROR_ARANGO_NO_INDEX);
  }

  TRI_hash_index_t* hashIndex = (TRI_hash_index_t*) idx;

  // convert the example (index is locked by lockRead)
  TRI_index_search_value_t searchValue;

  TRI_document_collection_t* document = trx.documentCollection();
  TRI_shaper_t* shaper = document->getShaper();  // PROTECTED by trx from above
  {
    v8::TryCatch tryCatch;
    int res = SetupSearchValue(&hashIndex->_paths, example, shaper, searchValue, args);

    if (res != TRI_ERROR_NO_ERROR) {
      if (res == TRI_RESULT_ELEMENT_NOT_FOUND) {
        TRI_V8_RETURN(EmptyResult(isolate));
      }
      tryCatch.ReThrow();
      return ;
    }
  }

  // find the matches
  TRI_vector_pointer_t list = TRI_LookupHashIndex(idx, &searchValue);
  DestroySearchValue(shaper->_memoryZone, searchValue);

  // convert result
  size_t total = TRI_LengthVectorPointer(&list);
  size_t count = 0;
  bool error = false;

  if (0 < total) {
    size_t s;
    size_t e;

    CalculateSkipLimitSlice(total, skip, limit, s, e);

    if (s < e) {
      for (size_t i = s;  i < e;  ++i) {
        v8::Handle<v8::Value> doc = WRAP_SHAPED_JSON(trx,
                                                     collection->_cid,
                                                     static_cast<TRI_doc_mptr_t*>(TRI_AtVectorPointer(&list, i))->getDataPtr());

        if (doc.IsEmpty()) {
          error = true;
          break;
        }
        else {
          documents->Set((uint32_t) count++, doc);
        }
      }
    }
  }

  // free data allocated by hash index result
  TRI_DestroyVectorPointer(&list);

  result->Set(TRI_V8_ASCII_STRING("total"), v8::Number::New(isolate, (double) total));
  result->Set(TRI_V8_ASCII_STRING("count"), v8::Number::New(isolate, (double) count));

  if (error) {
    TRI_V8_THROW_EXCEPTION_MEMORY();
  }

  TRI_V8_RETURN(result);
}

////////////////////////////////////////////////////////////////////////////////
/// @brief selects documents by example using a hash index
////////////////////////////////////////////////////////////////////////////////

static void JS_ByExampleHashIndex (const v8::FunctionCallbackInfo<v8::Value>& args) {
  v8::Isolate* isolate = args.GetIsolate();
  v8::HandleScope scope(isolate);

  TRI_vocbase_col_t const* col;
  col = TRI_UnwrapClass<TRI_vocbase_col_t>(args.Holder(), TRI_GetVocBaseColType());

  if (col == nullptr) {
    TRI_V8_THROW_EXCEPTION_INTERNAL("cannot extract collection");
  }

  TRI_THROW_SHARDING_COLLECTION_NOT_YET_IMPLEMENTED(col);

  SingleCollectionReadOnlyTransaction trx(new V8TransactionContext(true), col->_vocbase, col->_cid);

  int res = trx.begin();

  if (res != TRI_ERROR_NO_ERROR) {
    TRI_V8_THROW_EXCEPTION(res);
  }

  v8::Handle<v8::Object> err;

  // .............................................................................
  // inside a read transaction
  // .............................................................................

  trx.lockRead();

  ByExampleHashIndexQuery(trx, col, args);

  trx.finish(res);

  // .............................................................................
  // outside a write transaction
  // .............................................................................

}






////////////////////////////////////////////////////////////////////////////////
/// @brief selects documents by condition using a skiplist index
////////////////////////////////////////////////////////////////////////////////

static void JS_ByConditionSkiplist (const v8::FunctionCallbackInfo<v8::Value>& args) {
  std::string const signature("BY_CONDITION_SKIPLIST(<index>, <conditions>, <skip>, <limit>, <reverse>)");

  return ExecuteSkiplistQuery(args, signature, QUERY_CONDITION);
}

////////////////////////////////////////////////////////////////////////////////
/// @brief selects documents by example using a skiplist index
////////////////////////////////////////////////////////////////////////////////

static void JS_ByExampleSkiplist (const v8::FunctionCallbackInfo<v8::Value>& args) {
  std::string const signature("BY_EXAMPLE_SKIPLIST(<index>, <example>, <skip>, <limit>, <reverse>)");

  return ExecuteSkiplistQuery(args, signature, QUERY_EXAMPLE);
}

////////////////////////////////////////////////////////////////////////////////
/// @brief helper struct used when calculting checksums
////////////////////////////////////////////////////////////////////////////////

struct collection_checksum_t {
  collection_checksum_t (CollectionNameResolver const* resolver)
    : _resolver(resolver),
      _checksum(0) {
  }

  CollectionNameResolver const*  _resolver;
  TRI_string_buffer_t            _buffer;
  uint32_t                       _checksum;
};

////////////////////////////////////////////////////////////////////////////////
/// @brief callback for checksum calculation, WR = with _rid, WD = with data
////////////////////////////////////////////////////////////////////////////////

template<bool WR, bool WD> static bool ChecksumCalculator (TRI_doc_mptr_t const* mptr,
                                                           TRI_document_collection_t* document,
                                                           void* data) {
  // This callback is only called in TRI_DocumentIteratorDocumentCollection
  // and there we have an ongoing transaction. Therefore all master pointer
  // and data pointer accesses here are safe!
  TRI_df_marker_t const* marker = static_cast<TRI_df_marker_t const*>(mptr->getDataPtr());  // PROTECTED by trx in calling function TRI_DocumentIteratorDocumentCollection
  collection_checksum_t* helper = static_cast<collection_checksum_t*>(data);
  uint32_t localCrc;

  if (marker->_type == TRI_DOC_MARKER_KEY_DOCUMENT ||
      marker->_type == TRI_WAL_MARKER_DOCUMENT) {
    localCrc = TRI_Crc32HashString(TRI_EXTRACT_MARKER_KEY(mptr));  // PROTECTED by trx in calling function TRI_DocumentIteratorDocumentCollection
    if (WR) {
      localCrc += TRI_Crc32HashPointer(&mptr->_rid, sizeof(TRI_voc_rid_t));
    }
  }
  else if (marker->_type == TRI_DOC_MARKER_KEY_EDGE ||
           marker->_type == TRI_WAL_MARKER_EDGE) {
    // must convert _rid, _fromCid, _toCid into strings for portability
    localCrc = TRI_Crc32HashString(TRI_EXTRACT_MARKER_KEY(mptr));  // PROTECTED by trx in calling function TRI_DocumentIteratorDocumentCollection
    if (WR) {
      localCrc += TRI_Crc32HashPointer(&mptr->_rid, sizeof(TRI_voc_rid_t));
    }

    if (marker->_type == TRI_DOC_MARKER_KEY_EDGE) {
      TRI_doc_edge_key_marker_t const* e = reinterpret_cast<TRI_doc_edge_key_marker_t const*>(marker);
      string const extra = helper->_resolver->getCollectionNameCluster(e->_toCid) + TRI_DOCUMENT_HANDLE_SEPARATOR_CHR + string(((char*) marker) + e->_offsetToKey) +
                           helper->_resolver->getCollectionNameCluster(e->_fromCid) + TRI_DOCUMENT_HANDLE_SEPARATOR_CHR + string(((char*) marker) + e->_offsetFromKey);

      localCrc += TRI_Crc32HashPointer(extra.c_str(), extra.size());
    }
    else {
      triagens::wal::edge_marker_t const* e = reinterpret_cast<triagens::wal::edge_marker_t const*>(marker);
      string const extra = helper->_resolver->getCollectionNameCluster(e->_toCid) + TRI_DOCUMENT_HANDLE_SEPARATOR_CHR + string(((char*) marker) + e->_offsetToKey) +
                           helper->_resolver->getCollectionNameCluster(e->_fromCid) + TRI_DOCUMENT_HANDLE_SEPARATOR_CHR + string(((char*) marker) + e->_offsetFromKey);

      localCrc += TRI_Crc32HashPointer(extra.c_str(), extra.size());
    }
  }
  else {
    return true;
  }

  if (WD) {
    // with data
    void const* d = static_cast<void const*>(marker);

    TRI_shaped_json_t shaped;
    TRI_EXTRACT_SHAPED_JSON_MARKER(shaped, d);

    TRI_StringifyArrayShapedJson(document->getShaper(), &helper->_buffer, &shaped, false);  // PROTECTED by trx in calling function TRI_DocumentIteratorDocumentCollection
    localCrc += TRI_Crc32HashPointer(TRI_BeginStringBuffer(&helper->_buffer), TRI_LengthStringBuffer(&helper->_buffer));
    TRI_ResetStringBuffer(&helper->_buffer);
  }

  helper->_checksum += localCrc;

  return true;
}

////////////////////////////////////////////////////////////////////////////////
/// @brief calculates a checksum for the data in a collection
/// @startDocuBlock collectionChecksum
/// `collection.checksum(withRevisions, withData)`
///
/// The *checksum* operation calculates a CRC32 checksum of the keys
/// contained in collection *collection*.
///
/// If the optional argument *withRevisions* is set to *true*, then the
/// revision ids of the documents are also included in the checksumming.
///
/// If the optional argument *withData* is set to *true*, then the
/// actual document data is also checksummed. Including the document data in
/// checksumming will make the calculation slower, but is more accurate.
///
/// **Note**: this method is not available in a cluster.
///
/// @endDocuBlock
////////////////////////////////////////////////////////////////////////////////

static void JS_ChecksumCollection (const v8::FunctionCallbackInfo<v8::Value>& args) {
  v8::Isolate* isolate = args.GetIsolate();
  v8::HandleScope scope(isolate);

  if (ServerState::instance()->isCoordinator()) {
    // renaming a collection in a cluster is unsupported
    TRI_V8_THROW_EXCEPTION(TRI_ERROR_CLUSTER_UNSUPPORTED);
  }

  TRI_vocbase_col_t const* col;
  col = TRI_UnwrapClass<TRI_vocbase_col_t>(args.Holder(), TRI_GetVocBaseColType());

  if (col == nullptr) {
    TRI_V8_THROW_EXCEPTION_INTERNAL("cannot extract collection");
  }

  TRI_THROW_SHARDING_COLLECTION_NOT_YET_IMPLEMENTED(col);

  bool withRevisions = false;
  if (args.Length() > 0) {
    withRevisions = TRI_ObjectToBoolean(args[0]);
  }

  bool withData = false;
  if (args.Length() > 1) {
    withData = TRI_ObjectToBoolean(args[1]);
  }

  SingleCollectionReadOnlyTransaction trx(new V8TransactionContext(true), col->_vocbase, col->_cid);

  int res = trx.begin();

  if (res != TRI_ERROR_NO_ERROR) {
    TRI_V8_THROW_EXCEPTION(res);
  }

  TRI_document_collection_t* document = trx.documentCollection();

  if (trx.orderBarrier(trx.trxCollection()) == nullptr) {
    TRI_V8_THROW_EXCEPTION_MEMORY();
  }

  collection_checksum_t helper(trx.resolver());

  // .............................................................................
  // inside a read transaction
  // .............................................................................

  trx.lockRead();
  // get last tick
  string const rid = StringUtils::itoa(document->_info._revision);

  if (withData) {
    TRI_InitStringBuffer(&helper._buffer, TRI_CORE_MEM_ZONE);

    if (withRevisions) {
      TRI_DocumentIteratorDocumentCollection(&trx, document, &helper, &ChecksumCalculator<true, true>);
    }
    else {
      TRI_DocumentIteratorDocumentCollection(&trx, document, &helper, &ChecksumCalculator<false, true>);
    }

    TRI_DestroyStringBuffer(&helper._buffer);
  }
  else {
    if (withRevisions) {
      TRI_DocumentIteratorDocumentCollection(&trx, document, &helper, &ChecksumCalculator<true, false>);
    }
    else {
      TRI_DocumentIteratorDocumentCollection(&trx, document, &helper, &ChecksumCalculator<false, false>);
    }
  }

  trx.finish(res);

  // .............................................................................
  // outside a write transaction
  // .............................................................................

  v8::Handle<v8::Object> result = v8::Object::New(isolate);
  result->Set(TRI_V8_ASCII_STRING("checksum"), v8::Number::New(isolate, helper._checksum));
  result->Set(TRI_V8_ASCII_STRING("revision"), TRI_V8_STD_STRING(rid));

  TRI_V8_RETURN(result);
}

////////////////////////////////////////////////////////////////////////////////
/// @brief selects all edges for a set of vertices
/// @startDocuBlock edgeCollectionEdges
/// `edge-collection.edges(vertex)`
///
/// The *edges* operator finds all edges starting from (outbound) or ending
/// in (inbound) *vertex*.
///
/// `edge-collection.edges(vertices)`
///
/// The *edges* operator finds all edges starting from (outbound) or ending
/// in (inbound) a document from *vertices*, which must a list of documents
/// or document handles.
///
/// @EXAMPLES
///
/// @EXAMPLE_ARANGOSH_OUTPUT{edgeCollectionEdges}
/// ~ db._create("example");
///   db.relation.edges("vertex/1593622");
/// ~ db._drop("example");
/// @END_EXAMPLE_ARANGOSH_OUTPUT
///
/// @endDocuBlock
////////////////////////////////////////////////////////////////////////////////

static void JS_EdgesQuery (const v8::FunctionCallbackInfo<v8::Value>& args) {
  return EdgesQuery(TRI_EDGE_ANY, args);
}

////////////////////////////////////////////////////////////////////////////////
/// @brief selects all inbound edges
/// @startDocuBlock edgeCollectionInEdges
/// `edge-collection.inEdges(vertex)`
///
/// The *edges* operator finds all edges ending in (inbound) *vertex*.
///
/// `edge-collection.inEdges(vertices)`
///
/// The *edges* operator finds all edges ending in (inbound) a document from
/// *vertices*, which must a list of documents or document handles.
///
/// @EXAMPLES
///
/// @EXAMPLE_ARANGOSH_OUTPUT{edgeCollectionInEdges}
/// ~ db._create("example");
///   db.relation.inEdges("vertex/1528086");
///   db.relation.inEdges("vertex/1593622");
/// ~ db._drop("example");
/// @END_EXAMPLE_ARANGOSH_OUTPUT
///
/// @endDocuBlock
////////////////////////////////////////////////////////////////////////////////

static void JS_InEdgesQuery (const v8::FunctionCallbackInfo<v8::Value>& args) {
  return EdgesQuery(TRI_EDGE_IN, args);
}

////////////////////////////////////////////////////////////////////////////////
/// @brief selects the n first documents in the collection
////////////////////////////////////////////////////////////////////////////////

static void JS_FirstQuery (const v8::FunctionCallbackInfo<v8::Value>& args) {
  v8::Isolate* isolate = args.GetIsolate();
  v8::HandleScope scope(isolate);

  if (args.Length() > 1) {
    TRI_V8_THROW_EXCEPTION_USAGE("FIRST(<count>)");
  }

  int64_t count = 1;
  bool returnList = false;

  // if argument is supplied, we'll return a list - otherwise we simply return the first doc
  if (args.Length() == 1) {
    if (! args[0]->IsUndefined()) {
      count = TRI_ObjectToInt64(args[0]);
      returnList = true;
    }
  }

  if (count < 1) {
    TRI_V8_THROW_EXCEPTION_PARAMETER("invalid value for <count>");
  }

  TRI_vocbase_col_t const* col;
  col = TRI_UnwrapClass<TRI_vocbase_col_t>(args.Holder(), TRI_GetVocBaseColType());

  if (col == nullptr) {
    TRI_V8_THROW_EXCEPTION_INTERNAL("cannot extract collection");
  }

  SingleCollectionReadOnlyTransaction trx(new V8TransactionContext(true), col->_vocbase, col->_cid);

  int res = trx.begin();

  if (res != TRI_ERROR_NO_ERROR) {
    TRI_V8_THROW_EXCEPTION(res);
  }

  std::vector<TRI_doc_mptr_copy_t> documents;
  res = trx.readPositional(documents, 0, count);
  trx.finish(res);

  size_t const n = documents.size();

  if (returnList) {
    v8::Handle<v8::Array> result = v8::Array::New(isolate, (int) n);

    uint32_t j = 0;

    for (size_t i = 0; i < n; ++i) {
      v8::Handle<v8::Value> doc = WRAP_SHAPED_JSON(trx, col->_cid, documents[i].getDataPtr());

      if (doc.IsEmpty()) {
        // error
        TRI_V8_THROW_EXCEPTION_MEMORY();
      }

      result->Set(j++, doc);
    }

    TRI_V8_RETURN(result);
  }
  else {
    if (n == 0) {
      TRI_V8_RETURN_NULL();
    }

    v8::Handle<v8::Value> result = WRAP_SHAPED_JSON(trx, col->_cid, documents[0].getDataPtr());

    if (result.IsEmpty()) {
      TRI_V8_THROW_EXCEPTION_MEMORY();
    }

    TRI_V8_RETURN(result);
  }
}

////////////////////////////////////////////////////////////////////////////////
/// @brief queries the fulltext index
///
/// the caller must ensure all relevant locks are acquired and freed
////////////////////////////////////////////////////////////////////////////////

static void FulltextQuery (SingleCollectionReadOnlyTransaction& trx,
                           TRI_vocbase_col_t const* collection,
                           const v8::FunctionCallbackInfo<v8::Value>& args) {
  v8::Isolate* isolate = args.GetIsolate();
  v8::HandleScope scope(isolate);

  // expect: FULLTEXT(<index-handle>, <query>)
  if (args.Length() != 2) {
    TRI_V8_THROW_EXCEPTION_USAGE("FULLTEXT(<index-handle>, <query>)");
  }

  // extract the index
  TRI_index_t* idx = TRI_LookupIndexByHandle(isolate, trx.resolver(), collection, args[0], false);

  if (idx == nullptr) {
    return;
  }

  if (idx->_type != TRI_IDX_TYPE_FULLTEXT_INDEX) {
    TRI_V8_THROW_EXCEPTION(TRI_ERROR_ARANGO_NO_INDEX);
  }

  string const&& queryString = TRI_ObjectToString(args[1]);
  bool isSubstringQuery = false;

  TRI_fulltext_query_t* query = TRI_CreateQueryFulltextIndex(TRI_FULLTEXT_SEARCH_MAX_WORDS);

  if (! query) {
    TRI_V8_THROW_EXCEPTION_MEMORY();
  }

  int res = TRI_ParseQueryFulltextIndex(query, queryString.c_str(), &isSubstringQuery);

  if (res != TRI_ERROR_NO_ERROR) {
    TRI_FreeQueryFulltextIndex(query);

    TRI_V8_THROW_EXCEPTION(res);
  }

  TRI_fulltext_index_t* fulltextIndex = (TRI_fulltext_index_t*) idx;

  if (isSubstringQuery && ! fulltextIndex->_indexSubstrings) {
    TRI_FreeQueryFulltextIndex(query);

    TRI_V8_THROW_EXCEPTION(TRI_ERROR_NOT_IMPLEMENTED);
  }

  TRI_fulltext_result_t* queryResult = TRI_QueryFulltextIndex(fulltextIndex->_fulltextIndex, query);

  if (! queryResult) {
    TRI_V8_THROW_EXCEPTION_INTERNAL("internal error in fulltext index query");
  }

  if (trx.orderBarrier(trx.trxCollection()) == nullptr) {
    TRI_V8_THROW_EXCEPTION_MEMORY();
  }

  // setup result
  v8::Handle<v8::Object> result = v8::Object::New(isolate);

  v8::Handle<v8::Array> documents = v8::Array::New(isolate);
  result->Set(TRI_V8_ASCII_STRING("documents"), documents);

  bool error = false;

  for (uint32_t i = 0; i < queryResult->_numDocuments; ++i) {
    v8::Handle<v8::Value> doc = WRAP_SHAPED_JSON(trx, collection->_cid, ((TRI_doc_mptr_t const*) queryResult->_documents[i])->getDataPtr());

    if (doc.IsEmpty()) {
      error = true;
      break;
    }

    documents->Set(i, doc);
  }

  TRI_FreeResultFulltextIndex(queryResult);

  if (error) {
    TRI_V8_THROW_EXCEPTION_MEMORY();
  }

  TRI_V8_RETURN(result);
}

////////////////////////////////////////////////////////////////////////////////
/// @brief queries the fulltext index
/// @startDocuBlock collectionFulltext
/// `collection.fulltext(attribute, query)`
///
/// The *FULLTEXT* operator performs a fulltext search on the specified
/// *attribute* and the specified *query*.
///
/// Details about the fulltext query syntax can be found below.
///
/// @EXAMPLES
///
/// @EXAMPLE_ARANGOSH_OUTPUT{collectionFulltext}
/// ~ db._drop("emails");
/// ~ db._create("emails");
///   db.emails.ensureFulltextIndex("content").id;
///   db.emails.save({ content: "Hello Alice, how are you doing? Regards, Bob" });
///   db.emails.save({ content: "Hello Charlie, do Alice and Bob know about it?" });
///   db.emails.save({ content: "I think they don't know. Regards, Eve" });
///   db.emails.fulltext("content", "charlie,|eve").toArray();
/// ~ db._drop("emails");
/// @END_EXAMPLE_ARANGOSH_OUTPUT
/// @endDocuBlock
////////////////////////////////////////////////////////////////////////////////

static void JS_FulltextQuery (const v8::FunctionCallbackInfo<v8::Value>& args) {
  v8::Isolate* isolate = args.GetIsolate();
  v8::HandleScope scope(isolate);

  TRI_vocbase_col_t const* col;
  col = TRI_UnwrapClass<TRI_vocbase_col_t>(args.Holder(), TRI_GetVocBaseColType());

  if (col == nullptr) {
    TRI_V8_THROW_EXCEPTION_INTERNAL("cannot extract collection");
  }

  TRI_THROW_SHARDING_COLLECTION_NOT_YET_IMPLEMENTED(col);

  SingleCollectionReadOnlyTransaction trx(new V8TransactionContext(true), col->_vocbase, col->_cid);

  int res = trx.begin();

  if (res != TRI_ERROR_NO_ERROR) {
    TRI_V8_THROW_EXCEPTION(res);
  }

  v8::Handle<v8::Object> err;

  // .............................................................................
  // inside a read transaction
  // .............................................................................

  trx.lockRead();

  FulltextQuery(trx, col, args);

  trx.finish(res);

  // .............................................................................
  // outside a write transaction
  // .............................................................................
}

////////////////////////////////////////////////////////////////////////////////
/// @brief selects the n last documents in the collection
////////////////////////////////////////////////////////////////////////////////

static void JS_LastQuery (const v8::FunctionCallbackInfo<v8::Value>& args) {
  v8::Isolate* isolate = args.GetIsolate();
  v8::HandleScope scope(isolate);

  if (args.Length() > 1) {
    TRI_V8_THROW_EXCEPTION_USAGE("LAST(<count>)");
  }

  int64_t count = 1;
  bool returnList = false;

  // if argument is supplied, we'll return a list - otherwise we simply return the last doc
  if (args.Length() == 1) {
    if (! args[0]->IsUndefined()) {
      count = TRI_ObjectToInt64(args[0]);
      returnList = true;
    }
  }

  if (count < 1) {
    TRI_V8_THROW_EXCEPTION_PARAMETER("invalid value for <count>");
  }

  TRI_vocbase_col_t const* col;
  col = TRI_UnwrapClass<TRI_vocbase_col_t>(args.Holder(), TRI_GetVocBaseColType());

  if (col == nullptr) {
    TRI_V8_THROW_EXCEPTION_INTERNAL("cannot extract collection");
  }

  TRI_THROW_SHARDING_COLLECTION_NOT_YET_IMPLEMENTED(col);

  SingleCollectionReadOnlyTransaction trx(new V8TransactionContext(true), col->_vocbase, col->_cid);

  int res = trx.begin();

  if (res != TRI_ERROR_NO_ERROR) {
    TRI_V8_THROW_EXCEPTION(res);
  }

  vector<TRI_doc_mptr_copy_t> documents;
  res = trx.readPositional(documents, -1, count);
  trx.finish(res);

  size_t const n = documents.size();

  if (returnList) {
    v8::Handle<v8::Array> result = v8::Array::New(isolate, (int) n);

    uint32_t j = 0;

    for (size_t i = 0; i < n; ++i) {
      v8::Handle<v8::Value> doc = WRAP_SHAPED_JSON(trx, col->_cid, documents[i].getDataPtr());

      if (doc.IsEmpty()) {
        // error
        TRI_V8_THROW_EXCEPTION_MEMORY();
      }

      result->Set(j++, doc);
    }

    TRI_V8_RETURN(result);
  }
  else {
    if (n == 0) {
      TRI_V8_RETURN_NULL();
    }

    v8::Handle<v8::Value> result = WRAP_SHAPED_JSON(trx, col->_cid, documents[0].getDataPtr());

    if (result.IsEmpty()) {
      TRI_V8_THROW_EXCEPTION_MEMORY();
    }

    TRI_V8_RETURN(result);
  }
}

////////////////////////////////////////////////////////////////////////////////
/// @brief selects points near a given coordinate
///
/// the caller must ensure all relevant locks are acquired and freed
////////////////////////////////////////////////////////////////////////////////

static void NearQuery (SingleCollectionReadOnlyTransaction& trx,
                       TRI_vocbase_col_t const* collection,
                       const v8::FunctionCallbackInfo<v8::Value>& args) {
  v8::Isolate* isolate = args.GetIsolate();
  v8::HandleScope scope(isolate);

  // expect: NEAR(<index-id>, <latitude>, <longitude>, <limit>)
  if (args.Length() != 4) {
    TRI_V8_THROW_EXCEPTION_USAGE("NEAR(<index-handle>, <latitude>, <longitude>, <limit>)");
  }

  // extract the index
  TRI_index_t* idx = TRI_LookupIndexByHandle(isolate, trx.resolver(), collection, args[0], false);

  if (idx == nullptr) {
    return;
  }

  if (idx->_type != TRI_IDX_TYPE_GEO1_INDEX &&
      idx->_type != TRI_IDX_TYPE_GEO2_INDEX) {
    TRI_V8_THROW_EXCEPTION(TRI_ERROR_ARANGO_NO_INDEX);
  }

  // extract latitude and longitude
  double latitude = TRI_ObjectToDouble(args[1]);
  double longitude = TRI_ObjectToDouble(args[2]);

  // extract the limit
  TRI_voc_ssize_t limit = (TRI_voc_ssize_t) TRI_ObjectToDouble(args[3]);

  // setup result
  v8::Handle<v8::Object> result = v8::Object::New(isolate);

  v8::Handle<v8::Array> documents = v8::Array::New(isolate);
  result->Set(TRI_V8_ASCII_STRING("documents"), documents);

  v8::Handle<v8::Array> distances = v8::Array::New(isolate);
  result->Set(TRI_V8_ASCII_STRING("distances"), distances);

  GeoCoordinates* cors = TRI_NearestGeoIndex(idx, latitude, longitude, limit);

  if (cors != 0) {
    int res = StoreGeoResult(isolate, trx, collection, cors, documents, distances);

    if (res != TRI_ERROR_NO_ERROR) {
      TRI_V8_THROW_EXCEPTION(res);
    }
  }

  TRI_V8_RETURN(result);
}

////////////////////////////////////////////////////////////////////////////////
/// @brief selects points near a given coordinate
////////////////////////////////////////////////////////////////////////////////

static void JS_NearQuery (const v8::FunctionCallbackInfo<v8::Value>& args) {
  v8::Isolate* isolate = args.GetIsolate();
  v8::HandleScope scope(isolate);

  TRI_vocbase_col_t const* col;
  col = TRI_UnwrapClass<TRI_vocbase_col_t>(args.Holder(), TRI_GetVocBaseColType());

  if (col == nullptr) {
    TRI_V8_THROW_EXCEPTION_INTERNAL("cannot extract collection");
  }

  TRI_THROW_SHARDING_COLLECTION_NOT_YET_IMPLEMENTED(col);

  SingleCollectionReadOnlyTransaction trx(new V8TransactionContext(true), col->_vocbase, col->_cid);

  int res = trx.begin();

  if (res != TRI_ERROR_NO_ERROR) {
    TRI_V8_THROW_EXCEPTION(res);
  }

  // .............................................................................
  // inside a read transaction
  // .............................................................................

  trx.lockRead();

  NearQuery(trx, col, args);

  trx.finish(res);

  // .............................................................................
  // outside a write transaction
  // .............................................................................
}

////////////////////////////////////////////////////////////////////////////////
/// @brief selects all outbound edges
/// @startDocuBlock edgeCollectionOutEdges
/// `edge-collection.outEdges(vertex)`
///
/// The *edges* operator finds all edges starting from (outbound)
/// *vertices*.
///
/// `edge-collection.outEdges(vertices)`
///
/// The *edges* operator finds all edges starting from (outbound) a document
/// from *vertices*, which must a list of documents or document handles.
///
/// @EXAMPLES
///
/// @EXAMPLE_ARANGOSH_OUTPUT{edgeCollectionOutEdges}
/// ~ db._create("example");
///   db.relation.inEdges("vertex/1528086");
///   db.relation.inEdges("vertex/1593622");
/// ~ db._drop("example");
/// @END_EXAMPLE_ARANGOSH_OUTPUT
///
/// @endDocuBlock
////////////////////////////////////////////////////////////////////////////////

static void JS_OutEdgesQuery (const v8::FunctionCallbackInfo<v8::Value>& args) {
  return EdgesQuery(TRI_EDGE_OUT, args);
}

////////////////////////////////////////////////////////////////////////////////
/// @brief selects points within a given radius
///
/// the caller must ensure all relevant locks are acquired and freed
////////////////////////////////////////////////////////////////////////////////

static void WithinQuery (SingleCollectionReadOnlyTransaction& trx,
                         TRI_vocbase_col_t const* collection,
                         const v8::FunctionCallbackInfo<v8::Value>& args) {
  v8::Isolate* isolate = args.GetIsolate();
  v8::HandleScope scope(isolate);

  // expect: WITHIN(<index-handle>, <latitude>, <longitude>, <radius>)
  if (args.Length() != 4) {
    TRI_V8_THROW_EXCEPTION_USAGE("WITHIN(<index-handle>, <latitude>, <longitude>, <radius>)");
  }

  // extract the index
  TRI_index_t* idx = TRI_LookupIndexByHandle(isolate, trx.resolver(), collection, args[0], false);

  if (idx == nullptr) {
    return;
  }

  if (idx->_type != TRI_IDX_TYPE_GEO1_INDEX &&
      idx->_type != TRI_IDX_TYPE_GEO2_INDEX) {
    TRI_V8_THROW_EXCEPTION(TRI_ERROR_ARANGO_NO_INDEX);
  }

  // extract latitude and longitude
  double latitude = TRI_ObjectToDouble(args[1]);
  double longitude = TRI_ObjectToDouble(args[2]);

  // extract the radius
  double radius = TRI_ObjectToDouble(args[3]);

  // setup result
  v8::Handle<v8::Object> result = v8::Object::New(isolate);

  v8::Handle<v8::Array> documents = v8::Array::New(isolate);
  result->Set(TRI_V8_ASCII_STRING("documents"), documents);

  v8::Handle<v8::Array> distances = v8::Array::New(isolate);
  result->Set(TRI_V8_ASCII_STRING("distances"), distances);

  GeoCoordinates* cors = TRI_WithinGeoIndex(idx, latitude, longitude, radius);

  if (cors != 0) {
    int res = StoreGeoResult(isolate, trx, collection, cors, documents, distances);

    if (res != TRI_ERROR_NO_ERROR) {
      TRI_V8_THROW_EXCEPTION(res);
    }
  }

  TRI_V8_RETURN(result);
}

////////////////////////////////////////////////////////////////////////////////
/// @brief selects points within a given radius
////////////////////////////////////////////////////////////////////////////////

static void JS_WithinQuery (const v8::FunctionCallbackInfo<v8::Value>& args) {
  v8::Isolate* isolate = args.GetIsolate();
  v8::HandleScope scope(isolate);

  TRI_vocbase_col_t const* col;
  col = TRI_UnwrapClass<TRI_vocbase_col_t>(args.Holder(), TRI_GetVocBaseColType());

  if (col == nullptr) {
    TRI_V8_THROW_EXCEPTION_INTERNAL("cannot extract collection");
  }

  TRI_THROW_SHARDING_COLLECTION_NOT_YET_IMPLEMENTED(col);

  SingleCollectionReadOnlyTransaction trx(new V8TransactionContext(true), col->_vocbase, col->_cid);

  int res = trx.begin();

  if (res != TRI_ERROR_NO_ERROR) {
    TRI_V8_THROW_EXCEPTION(res);
  }

  v8::Handle<v8::Object> err;

  // .............................................................................
  // inside a read transaction
  // .............................................................................

  trx.lockRead();

  WithinQuery(trx, col, args);

  trx.finish(res);

  // .............................................................................
  // outside a read transaction
  // .............................................................................
}

// -----------------------------------------------------------------------------
// --SECTION--                                                            MODULE
// -----------------------------------------------------------------------------

// -----------------------------------------------------------------------------
// --SECTION--                                                  public functions
// -----------------------------------------------------------------------------

////////////////////////////////////////////////////////////////////////////////
/// @brief creates the query functions
////////////////////////////////////////////////////////////////////////////////

void TRI_InitV8Queries (v8::Isolate* isolate,
                        v8::Handle<v8::Context> context) {
  v8::HandleScope scope(isolate);

  TRI_GET_GLOBALS();
  TRI_ASSERT(v8g != nullptr);
  TRI_GET_GLOBAL(VocbaseColTempl, v8::ObjectTemplate);

  // .............................................................................
  // generate the TRI_vocbase_col_t template
  // .............................................................................

  TRI_AddMethodVocbase(isolate, VocbaseColTempl, TRI_V8_ASCII_STRING("ALL"), JS_AllQuery, true);
  TRI_AddMethodVocbase(isolate, VocbaseColTempl, TRI_V8_ASCII_STRING("ANY"), JS_AnyQuery, true);
  TRI_AddMethodVocbase(isolate, VocbaseColTempl, TRI_V8_ASCII_STRING("BY_CONDITION_SKIPLIST"), JS_ByConditionSkiplist, true);
  TRI_AddMethodVocbase(isolate, VocbaseColTempl, TRI_V8_ASCII_STRING("BY_EXAMPLE"), JS_ByExampleQuery, true);
  TRI_AddMethodVocbase(isolate, VocbaseColTempl, TRI_V8_ASCII_STRING("BY_EXAMPLE_HASH"), JS_ByExampleHashIndex, true);
  TRI_AddMethodVocbase(isolate, VocbaseColTempl, TRI_V8_ASCII_STRING("BY_EXAMPLE_SKIPLIST"), JS_ByExampleSkiplist, true);
  TRI_AddMethodVocbase(isolate, VocbaseColTempl, TRI_V8_ASCII_STRING("checksum"), JS_ChecksumCollection);
  TRI_AddMethodVocbase(isolate, VocbaseColTempl, TRI_V8_ASCII_STRING("EDGES"), JS_EdgesQuery, true);
  TRI_AddMethodVocbase(isolate, VocbaseColTempl, TRI_V8_ASCII_STRING("FIRST"), JS_FirstQuery, true);
  TRI_AddMethodVocbase(isolate, VocbaseColTempl, TRI_V8_ASCII_STRING("FULLTEXT"), JS_FulltextQuery, true);
  TRI_AddMethodVocbase(isolate, VocbaseColTempl, TRI_V8_ASCII_STRING("INEDGES"), JS_InEdgesQuery, true);
  TRI_AddMethodVocbase(isolate, VocbaseColTempl, TRI_V8_ASCII_STRING("LAST"), JS_LastQuery, true);
  TRI_AddMethodVocbase(isolate, VocbaseColTempl, TRI_V8_ASCII_STRING("NEAR"), JS_NearQuery, true);
  TRI_AddMethodVocbase(isolate, VocbaseColTempl, TRI_V8_ASCII_STRING("OUTEDGES"), JS_OutEdgesQuery, true);
  TRI_AddMethodVocbase(isolate, VocbaseColTempl, TRI_V8_ASCII_STRING("WITHIN"), JS_WithinQuery, true);

  TRI_AddMethodVocbase(isolate, VocbaseColTempl, TRI_V8_ASCII_STRING("mvccAny"), JS_MvccAny, true);
  TRI_AddMethodVocbase(isolate, VocbaseColTempl, TRI_V8_ASCII_STRING("mvccByExample"), JS_MvccByExample, true);
  TRI_AddMethodVocbase(isolate, VocbaseColTempl, TRI_V8_ASCII_STRING("mvccByExampleHash"), JS_MvccByExampleHashIndex, true);
  TRI_AddMethodVocbase(isolate, VocbaseColTempl, TRI_V8_ASCII_STRING("mvccChecksum"), JS_MvccChecksum, true);
  TRI_AddMethodVocbase(isolate, VocbaseColTempl, TRI_V8_ASCII_STRING("mvccEdges"), JS_MvccEdges, true);
  TRI_AddMethodVocbase(isolate, VocbaseColTempl, TRI_V8_ASCII_STRING("mvccInEdges"), JS_MvccInEdges, true);
  TRI_AddMethodVocbase(isolate, VocbaseColTempl, TRI_V8_ASCII_STRING("mvccOutEdges"), JS_MvccOutEdges, true);
  TRI_AddMethodVocbase(isolate, VocbaseColTempl, TRI_V8_ASCII_STRING("mvccFirst"), JS_MvccFirst, true);
  TRI_AddMethodVocbase(isolate, VocbaseColTempl, TRI_V8_ASCII_STRING("mvccLast"), JS_MvccLast, true);
  TRI_AddMethodVocbase(isolate, VocbaseColTempl, TRI_V8_ASCII_STRING("mvccNear"), JS_MvccNear, true);
  TRI_AddMethodVocbase(isolate, VocbaseColTempl, TRI_V8_ASCII_STRING("mvccWithin"), JS_MvccWithin, true);
}

// -----------------------------------------------------------------------------
// --SECTION--                                                       END-OF-FILE
// -----------------------------------------------------------------------------

// Local Variables:
// mode: outline-minor
// outline-regexp: "/// @brief\\|/// {@inheritDoc}\\|/// @page\\|// --SECTION--\\|/// @\\}"
// End:
