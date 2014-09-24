////////////////////////////////////////////////////////////////////////////////
/// @brief Aql, query context
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

#include "Aql/Query.h"
#include "Aql/ExecutionBlock.h"
#include "Aql/Executor.h"
#include "Aql/ExecutionEngine.h"
#include "Aql/ExecutionPlan.h"
#include "Aql/Optimizer.h"
#include "Aql/Parser.h"
#include "Basics/JsonHelper.h"
#include "Basics/json.h"
#include "Basics/tri-strings.h"
#include "Utils/AqlTransaction.h"
#include "Utils/Exception.h"
#include "Utils/V8TransactionContext.h"
#include "VocBase/vocbase.h"

using namespace triagens::aql;

// -----------------------------------------------------------------------------
// --SECTION--                                               static const values
// -----------------------------------------------------------------------------

static std::string StateNames[] = {
  "initialization",         // INITIALIZATION 
  "parsing",                // PARSING
  "ast optimization",       // AST_OPTIMIZATION
  "plan instanciation",     // PLAN_INSTANCIATION
  "plan optimization",      // PLAN_OPTIMIZATION
  "execution",              // EXECUTION
  "finalization"            // FINALIZATION
};

// make sure the state strings and the actual states match
static_assert(sizeof(StateNames) / sizeof(std::string) == static_cast<size_t>(ExecutionState::INVALID_STATE), 
              "invalid number of ExecutionState values");

// -----------------------------------------------------------------------------
// --SECTION--                                                    struct Profile
// -----------------------------------------------------------------------------

////////////////////////////////////////////////////////////////////////////////
/// @brief create a profile
////////////////////////////////////////////////////////////////////////////////
      
Profile::Profile () 
  : results(static_cast<size_t>(INVALID_STATE)),
    stamp(TRI_microtime()) {
}

////////////////////////////////////////////////////////////////////////////////
/// @brief enter a state
////////////////////////////////////////////////////////////////////////////////

void Profile::enter (ExecutionState state) {
  double const now = TRI_microtime();

  if (state != ExecutionState::INVALID_STATE) {
    // record duration of state
    results.push_back(std::make_pair(state, now - stamp)); 
  }

  // set timestamp
  stamp = now;
}

////////////////////////////////////////////////////////////////////////////////
/// @brief convert the profile to JSON
////////////////////////////////////////////////////////////////////////////////

TRI_json_t* Profile::toJson (TRI_memory_zone_t*) {
  triagens::basics::Json result(triagens::basics::Json::Array);
  for (auto& it : results) {
    result.set(StateNames[static_cast<int>(it.first)].c_str(), triagens::basics::Json(it.second));
  }
  return result.steal();
}

// -----------------------------------------------------------------------------
// --SECTION--                                                       class Query
// -----------------------------------------------------------------------------

// -----------------------------------------------------------------------------
// --SECTION--                                        constructors / destructors
// -----------------------------------------------------------------------------

////////////////////////////////////////////////////////////////////////////////
/// @brief creates a query
////////////////////////////////////////////////////////////////////////////////

Query::Query (TRI_vocbase_t* vocbase,
              char const* queryString,
              size_t queryLength,
              TRI_json_t* bindParameters,
              TRI_json_t* options)
  : _vocbase(vocbase),
    _executor(nullptr),
    _queryString(queryString),
    _queryLength(queryLength),
    _queryJson(),
    _bindParameters(bindParameters),
    _options(options),
    _collections(vocbase),
    _strings(),
    _ast(nullptr),
    _profile(nullptr),
    _state(INVALID_STATE),
    _plan(nullptr),
    _parser(nullptr),
    _trx(nullptr),
    _engine(nullptr) {

  TRI_ASSERT(_vocbase != nullptr);

  if (profiling()) {
    _profile = new Profile;
  }
  enterState(INITIALIZATION);
  
  _ast = new Ast(this);

  _strings.reserve(32);
}

Query::Query (TRI_vocbase_t* vocbase,
              triagens::basics::Json queryStruct,
              TRI_json_t* options)
  : _vocbase(vocbase),
    _executor(nullptr),
    _queryString(nullptr),
    _queryLength(0),
    _queryJson(queryStruct),
    _bindParameters(nullptr),
    _options(options),
    _collections(vocbase),
    _strings(),
    _ast(nullptr),
    _profile(nullptr),
    _state(INVALID_STATE),
    _plan(nullptr),
    _parser(nullptr),
    _trx(nullptr),
    _engine(nullptr) {

  TRI_ASSERT(_vocbase != nullptr);

  if (profiling()) {
    _profile = new Profile;
  }
  enterState(INITIALIZATION);

  _ast = new Ast(this);
  _strings.reserve(32);
}

////////////////////////////////////////////////////////////////////////////////
/// @brief destroys a query
////////////////////////////////////////////////////////////////////////////////

Query::~Query () {
  cleanupPlanAndEngine();
  if (_profile != nullptr) {
    delete _profile;
    _profile = nullptr;
  }

  if (_options != nullptr) {
    TRI_FreeJson(TRI_UNKNOWN_MEM_ZONE, _options);
    _options = nullptr;
  }

  if (_executor != nullptr) {
    delete _executor;
    _executor = nullptr;
  }

  if (_ast != nullptr) {
    delete _ast;
    _ast = nullptr;
  }

  // free strings
  for (auto it = _strings.begin(); it != _strings.end(); ++it) {
    TRI_FreeString(TRI_UNKNOWN_MEM_ZONE, const_cast<char*>(*it));
  }
}

// -----------------------------------------------------------------------------
// --SECTION--                                                    public methods
// -----------------------------------------------------------------------------

////////////////////////////////////////////////////////////////////////////////
/// @brief extract a region from the query
////////////////////////////////////////////////////////////////////////////////

std::string Query::extractRegion (int line, 
                                  int column) const {
  // note: line numbers reported by bison/flex start at 1, columns start at 0
  int currentLine   = 1;
  int currentColumn = 0;

  char c;
  char const* p = _queryString;

  while ((c = *p)) {
    if (currentLine > line || 
        (currentLine >= line && currentColumn >= column)) {
      break;
    }

    if (c == '\n') {
      ++p;
      ++currentLine;
      currentColumn = 0;
    }
    else if (c == '\r') {
      ++p;
      ++currentLine;
      currentColumn = 0;

      // eat a following newline
      if (*p == '\n') {
        ++p;
      }
    }
    else {
      ++currentColumn;
      ++p;
    }
  }

  // p is pointing at the position in the query the parse error occurred at
  TRI_ASSERT(p >= _queryString);

  size_t offset = static_cast<size_t>(p - _queryString);

  static int const   SNIPPET_LENGTH = 32;
  static char const* SNIPPET_SUFFIX = "...";

  if (_queryLength < offset + SNIPPET_LENGTH) {
    // return a copy of the region
    return std::string(_queryString + offset, _queryLength - offset);
  }

  // copy query part
  std::string result(_queryString + offset, SNIPPET_LENGTH);
  result.append(SNIPPET_SUFFIX);

  return result;
}

////////////////////////////////////////////////////////////////////////////////
/// @brief register an error
////////////////////////////////////////////////////////////////////////////////

void Query::registerError (int code,
                           char const* details) {

  TRI_ASSERT(code != TRI_ERROR_NO_ERROR);

  if (details == nullptr) {
    THROW_ARANGO_EXCEPTION(code);
  }
  else {
    THROW_ARANGO_EXCEPTION_PARAMS(code, details);
  }
}

////////////////////////////////////////////////////////////////////////////////
/// @brief prepare an AQL query, this is a preparation for execute, but 
/// execute calls it internally. The purpose of this separate method is
/// to be able to only prepare a query from JSON and then store it in the
/// QueryRegistry.
////////////////////////////////////////////////////////////////////////////////

QueryResult Query::prepare () {
  enterState(PARSING);

  try {
    std::unique_ptr<Parser> parser(new Parser(this));
    std::unique_ptr<ExecutionPlan> plan;

    if (_queryString != nullptr) {
      parser->parse();
      // put in bind parameters
      parser->ast()->injectBindParameters(_bindParameters);

      // optimize the ast
      enterState(AST_OPTIMIZATION);
      parser->ast()->optimize();
      // std::cout << "AST: " << triagens::basics::JsonHelper::toString(parser->ast()->toJson(TRI_UNKNOWN_MEM_ZONE, false)) << "\n";
    }

    // create the transaction object, but do not start it yet
    std::unique_ptr<AQL_TRANSACTION_V8> trx(new AQL_TRANSACTION_V8(_vocbase, _collections.collections()));

    if (_queryString != nullptr) {
      // we have an AST
      int res = trx->begin();

      if (res != TRI_ERROR_NO_ERROR) {
        return transactionError(res, *trx);
      }

      enterState(PLAN_INSTANCIATION);
      plan.reset(ExecutionPlan::instanciateFromAst(parser->ast()));
      if (plan.get() == nullptr) {
        // oops
        return QueryResult(TRI_ERROR_INTERNAL, "failed to create query execution engine");
      }
    }
    else {
      enterState(PLAN_INSTANCIATION);
      ExecutionPlan::getCollectionsFromJson(parser->ast(), _queryJson);

      // creating the plan may have produced some collections
      // we need to add them to the transaction now (otherwise the query will fail)
      int res = trx->addCollectionList(_collections.collections());
      
      if (res == TRI_ERROR_NO_ERROR) {
        res = trx->begin();
      }

      if (res != TRI_ERROR_NO_ERROR) {
        return transactionError(res, *trx);
      }

      // we have an execution plan in JSON format
      plan.reset(ExecutionPlan::instanciateFromJson(parser->ast(), _queryJson));
      if (plan.get() == nullptr) {
        // oops
        return QueryResult(TRI_ERROR_INTERNAL);
      }
    }

    // Run the query optimiser:
    enterState(PLAN_OPTIMIZATION);
    triagens::aql::Optimizer opt(maxNumberOfPlans());
    // get enabled/disabled rules
    opt.createPlans(plan.release(), getRulesFromOptions());  

    // Now plan and all derived plans belong to the optimizer
    plan.reset(opt.stealBest()); // Now we own the best one again
    TRI_ASSERT(plan.get() != nullptr);
    /* // for debugging of serialisation/deserialisation . . . * /
    auto JsonPlan = plan->toJson(parser->ast(),TRI_UNKNOWN_MEM_ZONE, true);
    auto JsonString = JsonPlan.toString();
    std::cout << "original plan: \n" << JsonString << "\n";

    auto otherPlan = ExecutionPlan::instanciateFromJson (parser->ast(),
                                                         JsonPlan);
    otherPlan->getCost(); 
    auto otherJsonString =
      otherPlan->toJson(parser->ast(), TRI_UNKNOWN_MEM_ZONE, true).toString(); 
    std::cout << "deserialised plan: \n" << otherJsonString << "\n";
    TRI_ASSERT(otherJsonString == JsonString); */

    enterState(EXECUTION);
    ExecutionEngine* engine(ExecutionEngine::instanciateFromPlan(trx.get(), this, plan.get()));

    // If all went well so far, then we keep _plan, _parser and _trx and
    // return:
    _plan = plan.release();
    _parser = parser.release();
    _trx = trx.release();
    _engine = engine;
    return QueryResult();
  }
  catch (triagens::arango::Exception const& ex) {
    cleanupPlanAndEngine();
    return QueryResult(ex.code(), getStateString() + ex.message());
  }
  catch (std::bad_alloc const&) {
    cleanupPlanAndEngine();
    return QueryResult(TRI_ERROR_OUT_OF_MEMORY, getStateString() + TRI_errno_string(TRI_ERROR_OUT_OF_MEMORY));
  }
  catch (std::exception const& ex) {
    cleanupPlanAndEngine();
    return QueryResult(TRI_ERROR_INTERNAL, getStateString() + ex.what());
  }
  catch (...) {
    cleanupPlanAndEngine();
    return QueryResult(TRI_ERROR_INTERNAL, getStateString() + TRI_errno_string(TRI_ERROR_INTERNAL));
  }
}

////////////////////////////////////////////////////////////////////////////////
/// @brief execute an AQL query 
////////////////////////////////////////////////////////////////////////////////

QueryResult Query::execute () {
  QueryResult res = prepare();
  if (res.code != TRI_ERROR_NO_ERROR) {
    return res;
  }

  // Now start the execution:
  try {
    triagens::basics::Json json(triagens::basics::Json::List, 16);
    triagens::basics::Json stats;

    AqlItemBlock* value;

    while (nullptr != (value = _engine->getSome(1, ExecutionBlock::DefaultBatchSize))) {
      auto doc = value->getDocumentCollection(0);
      size_t const n = value->size();
      // reserve space for n additional results at once
      json.reserve(n);

      for (size_t i = 0; i < n; ++i) {
        AqlValue val = value->getValue(i, 0);

        if (! val.isEmpty()) {
          json.add(val.toJson(_trx, doc)); 
        }
      }
      delete value;
    }

    stats = _engine->_stats.toJson();

    _trx->commit();
   
    cleanupPlanAndEngine();

    enterState(FINALIZATION); 

    QueryResult result(TRI_ERROR_NO_ERROR);
    result.json  = json.steal();
    result.stats = stats.steal(); 

    if (_profile != nullptr) {
      result.profile = _profile->toJson(TRI_UNKNOWN_MEM_ZONE);
    }

    return result;
  }
  catch (triagens::arango::Exception const& ex) {
    cleanupPlanAndEngine();
    return QueryResult(ex.code(), getStateString() + ex.message());
  }
  catch (std::bad_alloc const&) {
    cleanupPlanAndEngine();
    return QueryResult(TRI_ERROR_OUT_OF_MEMORY, getStateString() + TRI_errno_string(TRI_ERROR_OUT_OF_MEMORY));
  }
  catch (std::exception const& ex) {
    cleanupPlanAndEngine();
    return QueryResult(TRI_ERROR_INTERNAL, getStateString() + ex.what());
  }
  catch (...) {
    cleanupPlanAndEngine();
    return QueryResult(TRI_ERROR_INTERNAL, getStateString() + TRI_errno_string(TRI_ERROR_INTERNAL));
  }
}

////////////////////////////////////////////////////////////////////////////////
/// @brief close transaction to suspend query
////////////////////////////////////////////////////////////////////////////////

int Query::closeTransaction () {
  TRI_ASSERT(_trx != nullptr);
  _trx->commit();
  delete _trx;
  _trx = nullptr;
  return TRI_ERROR_NO_ERROR;
}

////////////////////////////////////////////////////////////////////////////////
/// @brief reopen transaction after suspend of query
////////////////////////////////////////////////////////////////////////////////

int Query::reOpenTransaction () {
  TRI_ASSERT(_trx == nullptr);
  _trx = new AQL_TRANSACTION_V8(_vocbase, _collections.collections());
  if (_trx == nullptr) {
    return TRI_ERROR_INTERNAL;
  }
  return _trx->begin();
}

////////////////////////////////////////////////////////////////////////////////
/// @brief parse an AQL query
////////////////////////////////////////////////////////////////////////////////

QueryResult Query::parse () {
  try {
    Parser parser(this);
    return parser.parse();
  }
  catch (triagens::arango::Exception const& ex) {
    return QueryResult(ex.code(), ex.message());
  }
  catch (...) {
    return QueryResult(TRI_ERROR_OUT_OF_MEMORY, TRI_errno_string(TRI_ERROR_OUT_OF_MEMORY));
  }
}

////////////////////////////////////////////////////////////////////////////////
/// @brief explain an AQL query
////////////////////////////////////////////////////////////////////////////////

QueryResult Query::explain () {
  enterState(PARSING);

  try {
    ExecutionPlan* plan = nullptr;
    Parser parser(this);

    parser.parse();
    // put in bind parameters
    parser.ast()->injectBindParameters(_bindParameters);

    enterState(AST_OPTIMIZATION);
    // optimize the ast
    parser.ast()->optimize();
    // std::cout << "AST: " << triagens::basics::JsonHelper::toString(parser.ast()->toJson(TRI_UNKNOWN_MEM_ZONE)) << "\n";

    // create the transaction object, but do not start it yet
    AQL_TRANSACTION_V8 trx(_vocbase, _collections.collections());

    // we have an AST
    int res = trx.begin();

    if (res != TRI_ERROR_NO_ERROR) {
      return transactionError(res, trx);
    }

    enterState(PLAN_INSTANCIATION);
    plan = ExecutionPlan::instanciateFromAst(parser.ast());
    if (plan == nullptr) {
      // oops
      return QueryResult(TRI_ERROR_INTERNAL);
    }

    // Run the query optimiser:
    enterState(PLAN_OPTIMIZATION);
    triagens::aql::Optimizer opt(maxNumberOfPlans());
    // get enabled/disabled rules
    opt.createPlans(plan, getRulesFromOptions());
      
    trx.commit();

    enterState(FINALIZATION);
      
    QueryResult result(TRI_ERROR_NO_ERROR);

    if (allPlans()) {
      triagens::basics::Json out(triagens::basics::Json::List);

      auto plans = opt.getPlans();
      for (auto it : plans) {
        TRI_ASSERT(it != nullptr);

        out.add(it->toJson(parser.ast(), TRI_UNKNOWN_MEM_ZONE, verbosePlans()));
      }
      
      result.json = out.steal();
    }
    else {
      // Now plan and all derived plans belong to the optimizer
      plan = opt.stealBest(); // Now we own the best one again
      TRI_ASSERT(plan != nullptr);

      result.json = plan->toJson(parser.ast(), TRI_UNKNOWN_MEM_ZONE, verbosePlans()).steal(); 

      delete plan;
    }
    
    return result;
  }
  catch (triagens::arango::Exception const& ex) {
    return QueryResult(ex.code(), getStateString() + ex.message());
  }
  catch (std::bad_alloc const&) {
    return QueryResult(TRI_ERROR_OUT_OF_MEMORY, getStateString() + TRI_errno_string(TRI_ERROR_OUT_OF_MEMORY));
  }
  catch (std::exception const& ex) {
    return QueryResult(TRI_ERROR_INTERNAL, getStateString() + ex.what());
  }
  catch (...) {
    return QueryResult(TRI_ERROR_INTERNAL, getStateString() + TRI_errno_string(TRI_ERROR_INTERNAL));
  }
}

////////////////////////////////////////////////////////////////////////////////
/// @brief get v8 executor
////////////////////////////////////////////////////////////////////////////////

Executor* Query::executor () {
  if (_executor == nullptr) {
    // the executor is a singleton per query
    _executor = new Executor;
  }

  TRI_ASSERT(_executor != nullptr);
  return _executor;
}

////////////////////////////////////////////////////////////////////////////////
/// @brief register a string
/// the string is freed when the query is destroyed
////////////////////////////////////////////////////////////////////////////////

char* Query::registerString (char const* p, 
                             size_t length,
                             bool mustUnescape) {

  if (p == nullptr) {
    THROW_ARANGO_EXCEPTION(TRI_ERROR_OUT_OF_MEMORY);
  }

  if (length == 0) {
    static char const* empty = "";
    // optimisation for the empty string
    return const_cast<char*>(empty);
  }

  char* copy = nullptr;
  if (mustUnescape) {
    size_t outLength;
    copy = TRI_UnescapeUtf8StringZ(TRI_UNKNOWN_MEM_ZONE, p, length, &outLength);
  }
  else {
    copy = TRI_DuplicateString2Z(TRI_UNKNOWN_MEM_ZONE, p, length);
  }

  if (copy == nullptr) {
    THROW_ARANGO_EXCEPTION(TRI_ERROR_OUT_OF_MEMORY);
  }

  try {
    _strings.push_back(copy);
  }
  catch (...) {
    THROW_ARANGO_EXCEPTION(TRI_ERROR_OUT_OF_MEMORY);
  }

  return copy;
}

////////////////////////////////////////////////////////////////////////////////
/// @brief register a string
/// the string is freed when the query is destroyed
////////////////////////////////////////////////////////////////////////////////

char* Query::registerString (std::string const& p,
                             bool mustUnescape) {

  return registerString(p.c_str(), p.length(), mustUnescape);
}

// -----------------------------------------------------------------------------
// --SECTION--                                                   private methods
// -----------------------------------------------------------------------------

////////////////////////////////////////////////////////////////////////////////
/// @brief fetch a boolean value from the options
////////////////////////////////////////////////////////////////////////////////

bool Query::getBooleanOption (char const* option, bool defaultValue) const {  
  if (! TRI_IsArrayJson(_options)) {
    return defaultValue;
  }

  TRI_json_t const* valueJson = TRI_LookupArrayJson(_options, option);
  if (! TRI_IsBooleanJson(valueJson)) {
    return defaultValue;
  }

  return valueJson->_value._boolean;
}

////////////////////////////////////////////////////////////////////////////////
/// @brief fetch a numeric value from the options
////////////////////////////////////////////////////////////////////////////////

double Query::getNumericOption (char const* option, double defaultValue) const {  
  if (! TRI_IsArrayJson(_options)) {
    return defaultValue;
  }

  TRI_json_t const* valueJson = TRI_LookupArrayJson(_options, option);
  if (! TRI_IsNumberJson(valueJson)) {
    return defaultValue;
  }

  return valueJson->_value._number;
}

////////////////////////////////////////////////////////////////////////////////
/// @brief neatly format transaction error to the user.
////////////////////////////////////////////////////////////////////////////////

QueryResult Query::transactionError (int errorCode, 
                                     AQL_TRANSACTION_V8 const& trx) const
{
  std::string err(TRI_errno_string(errorCode));

  auto detail = trx.getErrorData();
  if (detail.size() > 0) {
    err += std::string(" (") + detail + std::string(")");
  }

  err += std::string("\nwhile executing:\n") + _queryString + std::string("\n");

  return QueryResult(errorCode, err);
}

////////////////////////////////////////////////////////////////////////////////
/// @brief read the "optimizer.rules" section from the options
////////////////////////////////////////////////////////////////////////////////

std::vector<std::string> Query::getRulesFromOptions () const {
  std::vector<std::string> rules;

  if (! TRI_IsArrayJson(_options)) {
    return rules;
  }
  
  TRI_json_t const* optJson = TRI_LookupArrayJson(_options, "optimizer");

  if (! TRI_IsArrayJson(optJson)) {
    return rules;
  }
  
  TRI_json_t const* rulesJson = TRI_LookupArrayJson(optJson, "rules");

  if (! TRI_IsListJson(rulesJson)) {
    return rules;
  }
        
  size_t const n = TRI_LengthListJson(rulesJson);

  for (size_t i = 0; i < n; ++i) {
    TRI_json_t const* rule = static_cast<TRI_json_t const*>(TRI_AtVector(&rulesJson->_value._objects, i));

    if (TRI_IsStringJson(rule)) {
      rules.emplace_back(rule->_value._string.data, rule->_value._string.length - 1);
    }
  }

  return rules;
}

////////////////////////////////////////////////////////////////////////////////
/// @brief enter a new state
////////////////////////////////////////////////////////////////////////////////

void Query::enterState (ExecutionState state) {
  if (_profile != nullptr) {
    _profile->enter(_state);
  }

  // and adjust the state
  _state = state;
}

////////////////////////////////////////////////////////////////////////////////
/// @brief get a description of the query's current state
////////////////////////////////////////////////////////////////////////////////

std::string Query::getStateString () const {
  return "in state " + StateNames[_state] + ": ";
}

////////////////////////////////////////////////////////////////////////////////
/// @brief cleanup plan and engine for current query
////////////////////////////////////////////////////////////////////////////////

void Query::cleanupPlanAndEngine () {
  if (_engine != nullptr) {
    delete _engine;
    _engine = nullptr;
  }

  if (_trx != nullptr) {
    delete _trx;
    _trx = nullptr;
  }

  if (_parser != nullptr) {
    delete _parser;
    _parser = nullptr;
  }

  if (_plan != nullptr) {
    delete _plan;
    _plan = nullptr;
  }
}

// -----------------------------------------------------------------------------
// --SECTION--                                                       END-OF-FILE
// -----------------------------------------------------------------------------

// Local Variables:
// mode: outline-minor
// outline-regexp: "/// @brief\\|/// {@inheritDoc}\\|/// @page\\|// --SECTION--\\|/// @\\}"
// End: