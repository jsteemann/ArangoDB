module.define("org/arangodb/graph-blueprint", function(exports, module) {
/*jshint strict: false */

////////////////////////////////////////////////////////////////////////////////
/// @brief Graph functionality
///
/// @file
///
/// DISCLAIMER
///
/// Copyright 2010-2012 triagens GmbH, Cologne, Germany
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
/// @author Dr. Frank Celler, Lucas Dohmen
/// @author Copyright 2011-2012, triAGENS GmbH, Cologne, Germany
////////////////////////////////////////////////////////////////////////////////

var arangodb = require("org/arangodb"),
  is = require("org/arangodb/is"),
  common = require("org/arangodb/graph-common"),
  Edge = common.Edge,
  Graph = common.Graph,
  Vertex = common.Vertex,
  GraphArray = common.GraphArray,
  Iterator = common.Iterator,
  GraphAPI = require ("org/arangodb/api/graph").GraphAPI;


// -----------------------------------------------------------------------------
// --SECTION--                                                    public methods
// -----------------------------------------------------------------------------

////////////////////////////////////////////////////////////////////////////////
/// @brief changes a property of an edge
////////////////////////////////////////////////////////////////////////////////

Edge.prototype.setProperty = function (name, value) {
  var results,
    update = this._properties;

  update[name] = value;
  this._graph.emptyCachedPredecessors();

  results = GraphAPI.putEdge(this._graph._properties._key, this._properties._key, update);

  this._properties = results.edge;

  return name;
};

// -----------------------------------------------------------------------------
// --SECTION--                                                            Vertex
// -----------------------------------------------------------------------------

// -----------------------------------------------------------------------------
// --SECTION--                                                    public methods
// -----------------------------------------------------------------------------

////////////////////////////////////////////////////////////////////////////////
/// @brief inbound and outbound edges
////////////////////////////////////////////////////////////////////////////////

Vertex.prototype.edges = function (direction, labels) {
  var edge,
    edges = new GraphArray(),
    cursor;

  cursor = GraphAPI.postEdges(this._graph._vertices._database, this._graph._properties._key, this, {
    filter : { direction : direction, labels: labels }
  });

  while (cursor.hasNext()) {
    edge = new Edge(this._graph, cursor.next());
    edges.push(edge);
  }

  return edges;
};

////////////////////////////////////////////////////////////////////////////////
/// @brief inbound edges with given label
////////////////////////////////////////////////////////////////////////////////

Vertex.prototype.getInEdges = function () {
  var labels = Array.prototype.slice.call(arguments);
  return this.edges("in", labels);
};

////////////////////////////////////////////////////////////////////////////////
/// @brief outbound edges with given label
////////////////////////////////////////////////////////////////////////////////

Vertex.prototype.getOutEdges = function () {
  var labels = Array.prototype.slice.call(arguments);
  return this.edges("out", labels);
};

////////////////////////////////////////////////////////////////////////////////
/// @brief in- or outbound edges with given label
////////////////////////////////////////////////////////////////////////////////

Vertex.prototype.getEdges = function () {
  var labels = Array.prototype.slice.call(arguments);
  return this.edges("any", labels);
};

////////////////////////////////////////////////////////////////////////////////
/// @brief inbound edges
////////////////////////////////////////////////////////////////////////////////

Vertex.prototype.inbound = function () {
  return this.getInEdges();
};

////////////////////////////////////////////////////////////////////////////////
/// @brief outbound edges
////////////////////////////////////////////////////////////////////////////////

Vertex.prototype.outbound = function () {
  return this.getOutEdges();
};

////////////////////////////////////////////////////////////////////////////////
/// @brief changes a property of a vertex
////////////////////////////////////////////////////////////////////////////////

Vertex.prototype.setProperty = function (name, value) {
  var results,
    update = this._properties;

  update[name] = value;

  results = GraphAPI.putVertex(this._graph._properties._key, this._properties._key, update);

  this._properties = results.vertex;

  return name;
};

// -----------------------------------------------------------------------------
// --SECTION--                                      constructors and destructors
// -----------------------------------------------------------------------------

////////////////////////////////////////////////////////////////////////////////
/// @brief constructs a new graph object
////////////////////////////////////////////////////////////////////////////////

Graph.prototype.initialize = function (name, vertices, edges) {
  var results;

  if (is.notExisty(vertices) && is.notExisty(edges)) {
    results = GraphAPI.getGraph(name);
  } else {
    if (typeof vertices === 'object' && typeof vertices.name === 'function') {
      vertices = vertices.name();
    }
    if (typeof edges === 'object' && typeof edges.name === 'function') {
      edges = edges.name();
    }

    results = GraphAPI.postGraph({
      _key: name,
      vertices: vertices,
      edges: edges
    });
  }

  this._properties = results.graph;
  this._vertices = arangodb.db._collection(this._properties.edgeDefinitions[0].from[0]);
  this._edges = arangodb.db._collection(this._properties.edgeDefinitions[0].collection);

  // and dictionary for vertices and edges
  this._verticesCache = {};
  this._edgesCache = {};

  // and store the cashes
  this.predecessors = {};
  this.distances = {};

  return this;
};

// -----------------------------------------------------------------------------
// --SECTION--                                                    public methods
// -----------------------------------------------------------------------------

////////////////////////////////////////////////////////////////////////////////
/// @brief return all graphs
////////////////////////////////////////////////////////////////////////////////

Graph.getAll = function () {
  return GraphAPI.getAllGraphs();
};

////////////////////////////////////////////////////////////////////////////////
/// @brief static delete method
////////////////////////////////////////////////////////////////////////////////

Graph.drop = function (name) {
  GraphAPI.deleteGraph(name);
};

////////////////////////////////////////////////////////////////////////////////
/// @brief drops the graph, the vertices, and the edges
////////////////////////////////////////////////////////////////////////////////

Graph.prototype.drop = function () {
  GraphAPI.deleteGraph(this._properties._key);
};

////////////////////////////////////////////////////////////////////////////////
/// @brief saves an edge to the graph
////////////////////////////////////////////////////////////////////////////////

Graph.prototype._saveEdge = function(id, out_vertex_id, in_vertex_id, params) {
  var results;

  this.emptyCachedPredecessors();

  params._key = id;
  params._from = out_vertex_id;
  params._to = in_vertex_id;

  results = GraphAPI.postEdge(this._properties._key, params);
  return new Edge(this, results.edge);
};

////////////////////////////////////////////////////////////////////////////////
/// @brief saves a vertex to the graph
////////////////////////////////////////////////////////////////////////////////

Graph.prototype._saveVertex = function (id, params) {
  var results;

  if (is.existy(id)) {
    params._key = id;
  }

  results = GraphAPI.postVertex(this._properties._key, params);
  return new Vertex(this, results.vertex);
};

////////////////////////////////////////////////////////////////////////////////
/// @brief replace a vertex in the graph
////////////////////////////////////////////////////////////////////////////////

Graph.prototype._replaceVertex = function (id, data) {
  GraphAPI.putVertex(this._properties._key, id, data);
};

////////////////////////////////////////////////////////////////////////////////
/// @brief replace an edge in the graph
////////////////////////////////////////////////////////////////////////////////

Graph.prototype._replaceEdge = function (id, data) {
  GraphAPI.putEdge(this._properties._key, id, data);
};

////////////////////////////////////////////////////////////////////////////////
/// @brief returns a vertex given its id
////////////////////////////////////////////////////////////////////////////////

Graph.prototype.getVertex = function (id) {
  var results = GraphAPI.getVertex(this._properties._key, id);

  if (is.notExisty(results)) {
    return null;
  }

  return new Vertex(this, results.vertex);
};

////////////////////////////////////////////////////////////////////////////////
/// @brief returns an iterator for all vertices
////////////////////////////////////////////////////////////////////////////////

Graph.prototype.getVertices = function () {
  var cursor = GraphAPI.getVertices(this._vertices._database, this._properties._key, {}),
    graph = this,
    wrapper = function(object) {
      return new Vertex(graph, object);
    };

  return new Iterator(wrapper, cursor, "[vertex iterator]");
};

////////////////////////////////////////////////////////////////////////////////
/// @brief returns an edge given its id
////////////////////////////////////////////////////////////////////////////////

Graph.prototype.getEdge = function (id) {
  var results = GraphAPI.getEdge(this._properties._key, id);

  if (is.notExisty(results)) {
    return null;
  }

  return new Edge(this, results.edge);
};

////////////////////////////////////////////////////////////////////////////////
/// @brief returns an iterator for all edges
////////////////////////////////////////////////////////////////////////////////

Graph.prototype.getEdges = function () {
  var cursor = GraphAPI.getEdges(this._vertices._database, this._properties._key, {}),
    graph = this,
    wrapper = function(object) {
      return new Edge(graph, object);
    };
  return new Iterator(wrapper, cursor, "[edge iterator]");
};

////////////////////////////////////////////////////////////////////////////////
/// @brief removes a vertex and all in- or out-bound edges
////////////////////////////////////////////////////////////////////////////////

Graph.prototype.removeVertex = function (vertex) {
  this.emptyCachedPredecessors();
  GraphAPI.deleteVertex(this._properties._key, vertex._properties._key);
  vertex._properties = undefined;
};

////////////////////////////////////////////////////////////////////////////////
/// @brief removes an edge
////////////////////////////////////////////////////////////////////////////////

Graph.prototype.removeEdge = function (edge) {
  this.emptyCachedPredecessors();
  GraphAPI.deleteEdge(this._properties._key, edge._properties._key);
  this._edgesCache[edge._properties._id] = undefined;
  edge._properties = undefined;
};

// -----------------------------------------------------------------------------
// --SECTION--                                                    MODULE EXPORTS
// -----------------------------------------------------------------------------

exports.Edge = Edge;
exports.Graph = Graph;
exports.Vertex = Vertex;
exports.GraphArray = GraphArray;

require("org/arangodb/graph/algorithms-common");

// -----------------------------------------------------------------------------
// --SECTION--                                                       END-OF-FILE
// -----------------------------------------------------------------------------

// Local Variables:
// mode: outline-minor
// outline-regexp: "^\\(/// @brief\\|/// @addtogroup\\|// --SECTION--\\|/// @page\\|/// @}\\)"
// End:
});
