/*
 Copyright (c) 2013, Oracle and/or its affiliates. All rights
 reserved.
 
 This program is free software; you can redistribute it and/or
 modify it under the terms of the GNU General Public License
 as published by the Free Software Foundation; version 2 of
 the License.
 
 This program is distributed in the hope that it will be useful,
 but WITHOUT ANY WARRANTY; without even the implied warranty of
 MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 GNU General Public License for more details.
 
 You should have received a copy of the GNU General Public License
 along with this program; if not, write to the Free Software
 Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA
 02110-1301  USA
 */


/* Simple Twitter-like application for mysql-js
 *
 * This depends on the schema defined in tweet.sql
 *
 * It connects to the database using the properties defined in
 * tweet.properties.js
 *
 *
 */

'use strict';

var http   = require('http'),
    url    = require('url'),
    nosql  = require('../..'),
    udebug = unified_debug.getLogger("tweet.js"),
    getProperties = require("./tweet.properties.js").getProperties,
    getDefaultAdapter = require("./tweet.properties.js").getDefaultAdapter,
    adapter = getDefaultAdapter(),
    mainLoopComplete, parse_command, allOperations, operationMap;

//////////////////////////////////////////
/*  User Domain Object Constructors.

    These can be called in two different ways: first, by user code, 
    where they might take function arguments.  But secondly, when the
    mapping layer reads a row of a mapped table from the database, 
    it calls the constructor *with no arguments*.
    
    So the constructors must test whether the first argument === undefined. 
    If the constructor fails to do this, and overwrites the value just read
    from the database, the read operation will fail with error WCTOR.
*/

function Author(user_name, full_name) {
  if(user_name !== undefined) {
    this.user_name = user_name;
    this.full_name = full_name;
  }
}

function Tweet(author, message) {
  if(author !== undefined) {
    this.date_created = new Date();
    this.author = author;
    this.message = message;
  }
}

function HashtagEntry(tag, tweet) {
  if(tag !== undefined) {
    this.hashtag = tag;
    this.tweet_id = tweet.id;
  }
}

function Mention(at_user, tweet) {
  if(at_user !== undefined) {
    this.at_user = at_user;
    this.tweet_id = tweet.id;
  }
}

function Follow(follower, followed) {
  if(follower !== undefined) {
    this.follower = follower;
    this.followed = followed;
  }
}

Follow.prototype.toString = function() {
  return this.follower + " follows " + this.followed;
};

// OperationResponder is a common interface over 
// two sorts of responses (console and HTTP)

function ConsoleOperationResponder(operation) {
  this.operation  = operation;
  this.error      = false;
}

ConsoleOperationResponder.prototype.setError = function(e) {
 this.error = e;
};

ConsoleOperationResponder.prototype.write = function(x) {
  console.log(x);
};

ConsoleOperationResponder.prototype.close = function() {
    if(this.error) { console.log("Error", this.error); }
    else           { console.log("Success", this.operation.result); }
    this.operation.session.close(mainLoopComplete);
};


function HttpOperationResponder(operation, httpResponse) {
  this.operation    = operation;
  this.httpResponse = httpResponse;
  this.statusCode   = 200;
}

HttpOperationResponder.prototype.setError = function(e) { 
  this.statusCode = 500;
  this.operation.result = e;
};

HttpOperationResponder.prototype.write = function(x) {
  this.httpResponse.write(x);
};

HttpOperationResponder.prototype.close = function() {
  this.httpResponse.statusCode = this.statusCode;
  this.write(JSON.stringify(this.operation.result));
  this.write("\n");
  this.httpResponse.end();
  this.operation.session.close();
}; 

// ======== Callbacks run at various phases of a database operation ======= //

/* mainLoopComplete() callback.  This runs after the single operation 
   given on the command line, or after the web server has shut down.
*/
function mainLoopComplete() {
  console.log("--FINISHED--");
  process.exit(0);
}

/* onComplete(): when a single database operation is complete.
   * error       : error from operation; will be stored in operation.latestError
   * resultData  : Operation result; for a find(), this will be the found item
   * operation   : Operation just completed
   * nextCallback: if supplied, will be called with operation as argument
*/
function onComplete(error, resultData, operation, nextCallback) {
  udebug.log("onComplete :", error, ":", resultData, ":", operation.name);
  operation.latestError = error;
  if(resultData)   {   operation.result = resultData;       }
  if(error)        {   operation.responder.setError(error); }
  if(nextCallback) {   nextCallback(operation);             }
}

/* onFinal(): When an Operation is complete, close its responder. 
*/
function onFinal(operation) {
  operation.responder.close();
}

/* onOpenSession(): bridge from openSession() to operation.run()
*/
function onOpenSession(error, session, operation) {
  operation.latestError = error;
  operation.session = session;
  if(session && ! error) {
    operation.run();
  }
  else {
    onComplete(error, null, operation, onFinal);
  }
}

/* doRollback(): roll back a failed transaction
*/
function doRollback(operation) {
  operation.session.currentTransaction().rollback(function() {onFinal(operation);});
}

/* Increment a stored counter
   This uses session.load() to read in an object from the database,
   increments the counter, then uses session.update() to save it back.
*/
function increment(operation, object, counterProperty, callback) {
  function onRead(error) {
    operation.latestError = error;
    if(error) {
      callback(error, operation);
    }
    else {
      object[counterProperty] += 1;
      operation.session.update(object, callback, operation);
    }
  }

  operation.session.load(object, onRead);
}


/* extractTags() returns arrays of #hashtags and @mentions present in a message 
*/
function extractTags(message) {
  var words, word, tags, tag;
  tags = {"hash" : [], "at" : [] };
  words = message.split(/\s+/);
  word = words.pop();
  while(word !== undefined) {
    if(word.charAt(0) == "#") {
      tag = /#(\w+)/.exec(word)[1];
      if(tags.hash.indexOf(tag) == -1) {
        tags.hash.push(tag);
      }
    }
    else if(word.charAt(0) == "@") {
      tag = /@(\w+)/.exec(word)[1];
      if(tags.at.indexOf(tag) == -1) {
        tags.at.push(tag);
      }
    }
    word = words.pop();
  }
  return tags;
}


////////////////////////////////
// BASIC OPERATIONS
//
// All actions (insert, delete, start the http server) are represented
// by instances of Operation.
// 
// Each instance has several properties defined in the Operation constructor,
// and a run() method.  
//
// A multi-step operation might have additional properties used to pass data
// from one step to the next
////////////////////////////////

function Operation() {
  this.run          = {};    // a run() method
  this.session      = {};    // This will be set before control reaches run()
  this.latestError  = {};    // This must be set in every callback
  this.responder    = {};    // OperationResponder for this operation
  this.result       = {};    // Result object to be returned to the user
  this.data         = {};    // HTTP POST data
  this.isServer     = false; // True only for the start-HTTP-server operation
}


// Each Operation constructor takes (params, data), where "params" is an array
// of URL or command-line parameters.  "data" is the HTTP POST data.
// In the command line version, the POST data comes from the final parameter
//
// An Operation Constructor also has a signature property, which is an array
// of [ verb, noun, parameters, data ] used in the API.  "verb" is an HTTP
// verb; noun and parameters are separated by slashes in the URL.


/* Add a user 
*/
function AddUserOperation(params, data) {
  var author = new Author(params[0], data);
  Operation.call(this);    /* inherit */
  
  this.run = function() {
    this.session.persist(author, onComplete, author, this, onFinal);
  };
}
AddUserOperation.signature = [ "put", "user", "<user_name>", " << Full Name >>" ];


/* Profile a user based on username.
   This calls find(), then stores the result in self.object
*/
function LookupUserOperation(params, data) {
  Operation.call(this);    /* inherit */
  
  this.run = function() {
    this.session.find(Author, params[0], onComplete, this, onFinal);
  };
}
LookupUserOperation.signature = [ "get", "user", "<user_name>" ]; 


/* Delete a user, with cascading delete of tweets, mentions, and followers
*/
function DeleteUserOperation(params, data) { 
  Operation.call(this);    /* inherit */
  var author_name = params[0];

  this.run = function() {
    this.session.remove(Author, author_name, onComplete, 
                        {"deleted": author_name},  this, onFinal);
  };
}
DeleteUserOperation.signature = [ "delete", "user", "<user_name>" ];


/* Insert a tweet.
     - Start a transaction.
     - Persist the tweet.  After persist, the tweet's auto-increment id 
       is available (and will be used by the HashtagEntry and Mention constructors).
     - Create & persist #hashtag & @user records (all in a single batch).
     - Increment the author's tweet count.
     - Then commit the transaction. 
*/
function InsertTweetOperation(params, data) {
  Operation.call(this);    /* inherit */

  var session;
  var author = params[0];
  var message = data;
  var tweet = new Tweet(author, message);

  function doCommit(error, self) {
    if(error) {
      doRollback(self);
    }
    else {
      session.currentTransaction().commit(onComplete, null, self, onFinal);
    }
  }

  function incrementTweetCount(self) {
    var authorRecord = new Author(author);  // Increment author's tweet count 
    increment(self, authorRecord, "tweets", doCommit);
  }

  function onTweetCreateTagEntries(self) {
    var batch, tags, tag, tagEntry, authorRecord;

    if(self.latestError) {
      doRollback(self);
    }
    else {
      /* Store all #hashtag and @mention entries in a single batch */

      tags = extractTags(message);
      batch = session.createBatch();

      tag = tags.hash.pop();   // # hashtags
      while(tag !== undefined) {
        tagEntry = new HashtagEntry(tag, tweet);
        batch.persist(tagEntry, onComplete, null, self);
        tag = tags.hash.pop();
      }
      
      tag = tags.at.pop();   // @ mentions
      while(tag != undefined) {
        tagEntry = new Mention(tag, tweet);
        batch.persist(tagEntry, onComplete, null, self);
        tag = tags.at.pop();
      }

      batch.execute(onComplete, null, self, incrementTweetCount);
    }
  }

  this.run = function() {   /* Start here */
    session = this.session;         
    session.currentTransaction().begin();
    session.persist(tweet, onComplete, tweet, this, onTweetCreateTagEntries);
  };
}
InsertTweetOperation.signature = [ "post", "tweet", "<author>", " << Message >>" ];


/* Delete a tweet.
   Relies on cascading delete to remove hashtag entries.
*/
function DeleteTweetOperation(params, data) { 
  Operation.call(this);    /* inherit */
  var tweet_id = params[0];

  this.run = function() {
    this.session.remove(Tweet, tweet_id, onComplete, 
                        {"deleted": tweet_id},  this, onFinal);
  };
}
DeleteTweetOperation.signature = [ "delete", "tweet", "<tweet_id>" ];


/* Get a tweet by id.
*/
function ReadTweetOperation(params, data) {
  Operation.call(this);

  this.run = function() {
    this.session.find(Tweet, params[0], onComplete, this, onFinal);
  };
}
ReadTweetOperation.signature = [ "get", "tweet", "<tweet_id>" ];


/* Make user A a follower of user B
*/
function FollowOperation(params, data) {
  Operation.call(this);
  
  this.run = function() {
    var record = new Follow(params[0], params[1]);
    this.session.persist(Follow, record, onComplete, record.toString(), 
                         this, onFinal);
  };
}
FollowOperation.signature = [ "put", "follow", "<user_follower> <user_followed>"];


/* Common callback for queries on field=value
*/
function buildQueryEq(error, query, self, qparams, nextCallback) {
  var field;
  for(field in qparams) {
    if(qparams.hasOwnProperty(field) && query[field]) {
      query.where(query[field].eq(query.param(field)));
    }
  }
  query.execute(qparams, nextCallback, self, onFinal);  
}


/* Who follows a user?
*/
function FollowersOperation(params, data) {
  Operation.call(this);
  
  this.run = function() {
    var qp = {"followed" : params[0] };
    this.session.createQuery(Follow, buildQueryEq, this, qp, onComplete);
  };
}
FollowersOperation.signature = [ "get", "followers", "<user_name>" ];


/* Whom does a user follow?
*/
function FollowingOperation(params, data) {
  Operation.call(this);
  
  this.run = function() {
    var qp = {"follower" : params[0] };
    this.session.createQuery(Follow, buildQueryEq, this, qp, onComplete);
  };
}
FollowingOperation.signature = [ "get", "following" , "<user_name>" ];


/* Get the N most recent tweets
*/
function RecentTweetsOperation(params, data) {
  Operation.call(this);
  var limit = params && params[0] ? Number(params[0]) : 20;
  
  function buildQuery(error, query, self) {
    var params = {"zero" : 0, "order" : "desc", "limit" : limit};
    /* use id > 0 to coerce a descending index scan on id */
    query.where(query.id.gt(query.param("zero")));
    query.execute(params, onComplete, self, onFinal);
  }

  this.run = function() {
    this.session.createQuery(Tweet, buildQuery, this);
  };
}
RecentTweetsOperation.signature = [ "get", "tweets-recent" , "<count>" ];


/* Last 20 tweets from a user
*/
function TweetsByUserOperation(params, data) {
  Operation.call(this);

  this.run = function() {
    var qp = {"author" : params[0] , "order" : "desc" , "limit" : 20 };
    this.session.createQuery(Tweet, buildQueryEq, this, qp, onComplete);
  };
}
TweetsByUserOperation.signature = [ "get" , "tweets-by" , "<user_name>" ];


/* Common callback for @user and #hashtag queries 
*/
function fetchTweetsInBatch(error, scanResults, operation) {
  var batch, r;
  var resultData = [];
  
  function addTweetToResults(e, tweet) {
    if(tweet && ! e) resultData.push(tweet);
  }
  
  if(scanResults.length && ! error) {
    batch = operation.session.createBatch();
    r = scanResults.shift();
    while(r) {
      batch.find(Tweet, r.tweet_id, addTweetToResults);
      r = scanResults.shift();
    }
    batch.execute(onComplete, resultData, operation, onFinal);  
  }
  else {
    onComplete(error, null, operation, onFinal);  
  }
}


/* Last 20 tweets @user
*/
function TweetsAtUserOperation(params, data) {
  Operation.call(this);
  
  this.run = function() {
    var tag = params[0];
    if(tag.charAt(0) == "@") {    tag = tag.substring(1);   }
    var qp = {"at_user" : tag, "order" : "desc" , "limit" : 20 };
    this.session.createQuery(Mention, buildQueryEq, this, qp, fetchTweetsInBatch);
  };
}
TweetsAtUserOperation.signature = [ "get" , "tweets-at" , "<user_name>" ];


/* Last 20 tweets with hashtag
*/
function TweetsByHashtagOperation(params, data) {
  Operation.call(this);
  
  this.run = function() {
    var tag = params[0];
    if(tag.charAt(0) == "#") {    tag = tag.substring(1);   }
    var qp = {"hashtag" : tag, "order" : "desc" , "limit" : 20 };
    this.session.createQuery(HashtagEntry, buildQueryEq, this, qp, fetchTweetsInBatch);
  };
}
TweetsByHashtagOperation.signature = [ "get" , "tweets-about", "<hashtag>" ];


/* The web server Operation is just slightly different; 
   it defines isServer = true, 
   and it defines a runServer() method, which takes the SessionFactory.
*/
function RunWebServerOperation(cli_params, cli_data) {
  var port = cli_params[0];
  var sessionFactory;
  this.isServer = true;
  
  function serverRequestLoop(request, response) {
    var params, data;

    function hangup(code) { 
      response.statusCode = code;
      response.end();
    }

    function runOperation() {
      var operation = parse_command(request.method, params, data);
      if(operation && ! operation.isServer) {
        operation.responder = new HttpOperationResponder(operation, response);
        sessionFactory.openSession(null, onOpenSession, operation);  
      } 
      else hangup(400);
    }

    data = "";
    params = url.parse(request.url).pathname.split("/");
    params.shift();
    
    request.setEncoding('utf8');
    function gatherData(chunk) {    data += chunk;    }
    request.on('data', gatherData);  
    request.on('end', runOperation);
  }
  
  this.runServer = function(_sessionFactory) {
    sessionFactory = _sessionFactory;
    http.createServer(serverRequestLoop).listen(port);
    console.log("Server started on port", port);  
  };
}
RunWebServerOperation.signature = [ "start" , "server" , "<server_port_number>" ];


function HelpOperation(params, data) {
  Operation.call(this);
  
  this.run = function() {
    this.responder.write(operationMap.http_help);
    this.responder.close();
  };
}
HelpOperation.signature = [ "get" , "help" ];


function parse_command(method, params, data) {
  var verb, noun, opIdx, opConstructor, operation;
  verb = method.toLocaleLowerCase();
  noun = params.shift();
  operation = null;
  udebug.log("parse_command", verb, noun, params);
  if(operationMap.verbs[verb] && operationMap.verbs[verb][noun]) {
    opIdx = operationMap.verbs[verb][noun][0];
    opConstructor = allOperations[opIdx];
    if(opConstructor) {
      operation = {};
      opConstructor.call(operation, params, data);
    }
  }
  
  return operation;
}


function get_cmdline_args() { 
  var i, k, spaces, val, verb, operation;
  var cmdList = [];
  var usageMessage = 
    "Usage: node tweet {options} {command} {command arguments}\n" +
    "         -a <adapter>:  run using the named adapter (default: "+adapter+")\n"+
    "         -h or --help: print this message\n" +
    "         -d or --debug: set the debug flag\n" +
    "               --detail: set the detail debug flag\n" +
    "               -df <file>: enable debug output from <file>\n" +
    "\n" +
    "  COMMANDS:\n" + operationMap.cli_help;
  
  for(i = 2; i < process.argv.length ; i++) {
    val = process.argv[i];
    switch (val) {
      case '-a':
        adapter = process.argv[++i];
        break;
      case '--debug':
      case '-d':
        unified_debug.on();
        unified_debug.level_debug();
        break;
      case '--detail':
        unified_debug.on();
        unified_debug.level_detail();
        break;
      case '-df':
        unified_debug.set_file_level(process.argv[++i], 5);
        break;
      case '--help':
      case '-h':
        break;
      default:
        cmdList.push(val);
    }
  }

  if(cmdList.length) {
    verb = cmdList.shift();
    /* Use final command line argument as "data" */    
    operation = parse_command(verb, cmdList, cmdList[cmdList.length-1]);
  }

  if(operation) {
    return operation;
  }
   
  console.log(usageMessage);
  process.exit(1);
}


/* Run a single operation specified on the command line
*/
function runCmdlineOperation(err, sessionFactory, operation) {
  if(err) {
    console.log(err);
    process.exit(1);
  }

  if(operation.isServer) {
    operation.runServer(sessionFactory);
  }
  else {
    operation.responder = new ConsoleOperationResponder(operation);
    sessionFactory.openSession(null, onOpenSession, operation);
  }
}

var allOperations = [ 
  AddUserOperation,
  LookupUserOperation,
  DeleteUserOperation,
  InsertTweetOperation,
  ReadTweetOperation,
  DeleteTweetOperation,
  FollowOperation,
  FollowersOperation, 
  FollowingOperation,
  TweetsByUserOperation,
  TweetsAtUserOperation,
  TweetsByHashtagOperation,
  RecentTweetsOperation,
  RunWebServerOperation,
  HelpOperation
];

function prepareOperationMap() {
  var idx, sig, verb, data;
  operationMap = {};
  operationMap.cli_help = "";
  operationMap.http_help = "";
  operationMap.verbs = { "get":{}, "post":{}, "put":{}, "delete":{} , "start":{} };
  
  for(idx in allOperations) {
    if(allOperations.hasOwnProperty(idx)) {
      sig = allOperations[idx].signature;
      operationMap.verbs[sig[0]][sig[1]] = [ idx, sig[2], sig[3] ];
      operationMap.cli_help += "         " + sig.join(" ") + "\n";
      verb = sig.shift().toLocaleUpperCase();
      data = sig[2];
      operationMap.http_help += verb +" /"+ sig[0] +"/"+ sig[1];
      operationMap.http_help += data ? data + "\n" : "\n";
    }
  }
}

// *** Main program starts here ***

/* Global Variable Declarations */
var mappings, dbProperties, operation;

prepareOperationMap();

/* This may have the side effect of changing the adapter */
operation = get_cmdline_args();

/* Connection Properties */
dbProperties = getProperties(adapter);

// Map SQL Tables to JS Constructors using default mappings
mappings = [];
mappings.push(new nosql.TableMapping('tweet').applyToClass(Tweet));
mappings.push(new nosql.TableMapping('author').applyToClass(Author));
mappings.push(new nosql.TableMapping('hashtag').applyToClass(HashtagEntry));
mappings.push(new nosql.TableMapping('follow').applyToClass(Follow));
mappings.push(new nosql.TableMapping('mention').applyToClass(Mention));

nosql.connect(dbProperties, mappings, runCmdlineOperation, operation);

