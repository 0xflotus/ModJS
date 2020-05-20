# ModJS
A Javascript Module for KeyDB and Redis.

ModJS allows you to extend Redis and KeyDB with new functionality implemented in JavaScript.  ModJS uses the V8 JIT engine so complex scripts can execute significantly faster than their Lua equivalents.  In addition ModJS supports many node.js modules offering extensive library support for common tasks.

## Quick Start Guide
There are two ways to use ModJS, the first is similar to Lua with the EVALJS Command:

    > EVALJS "redis.call('get', 'testkey')"
    
While EVALJS is quick and easy, a much more powerful method exists in the form of startup scripts. 
In a startup script you can define your own custom commands and call them from any client as though they were built-in.  In addition,
these commands can skip the parsing step of EVALJS and so will execute much faster.

### Adding a Command

All commands must be defined at the time the module is loaded in a startup script.  A startup script is simply a javascript file who's path is passed to ModJS at load time.

Below we create an example script named startup.js which defines a new command named "concat".  This command fetches two keys and returns the string concatenation of the two values.
We then register this function with the server so that it will be directly callable from Redis or KeyDB clients.

    function concat(key1, key2) {
        var str1 = redis.call('get', key1);
        var str2 = redis.call('get', key2);
        return str1 + str2;
    }
    
    keydb.register(concat);

*Note: The redis and keydb objects may be used interchangebly.*

To run this script on startup simply add the path as a module parameter, e.g. ``loadmodule /path/to/modjs.so /path/to/startup.js``

We may now use this command from any client.  E.g.:

    127.0.0.1:6379> set keyA foo
    OK
    127.0.0.1:6379> set keyB bar
    OK
    127.0.0.1:6379> concat keyA keyB
    "foobar"

### Importing scripts from npm

The above examples were simple enough not to require external libraries, however for more complex tasks it may be desireable to import modules fetched via npm.  ModJS implements the require() api with similar semantics to node.js.  

In this example we will use the popular lodash library, installed with: ``npm install loadash``.  Below we've updated our example script to use the camelCase() function in lodash:

    var _ = require("lodash")

    function concat(key1, key2) {
        var str1 = redis.call('get', key1);
        var str2 = redis.call('get', key2);
        return _.camelCase(str1 + " " + str2)
    }
    
    keydb.register(concat);

The lodash module is imported with require() as it would be in a node.js script.  Note that require() will search for modules starting from the working directory of Redis or KeyDB.

Once loaded this new script will concatenate the two strings using camel case.


# Compiling ModJS

ModJS requires you to first build V8, as a result we recommend using a pre-compiled docker image.  However if you wish to compile ModJS first follow the instructions to download and build V8 here: https://v8.dev/docs/build

Once you have built the core V8 library we must then build the monolith with the following command:

    /path/to/v8/$ ninja -C out.gn/x64.release.sample v8_monolith 
    
If V8 compiled successfully you are now ready to build ModJS.  ModJS can be built with one line:

    make V8_PATH=/path/to/v8
    

