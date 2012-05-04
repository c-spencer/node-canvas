
/**
 * Module dependencies.
 */

var Canvas = require('../')
  , Image = Canvas.Image
  , assert = require('assert');

var png = __dirname + '/fixtures/clock.png';

var oom_png = __dirname + '/fixtures/oom.png';
var cmyk_jpeg = __dirname + '/fixtures/cmyk.jpg';
var corrupt_jpeg = __dirname + '/fixtures/corrupt.jpg';

module.exports = {
  'tset Image': function(){
    assert.ok(Image instanceof Function);
  },

  'test Image#onload': function(){
    var img = new Image
      , n = 0;

    assert.strictEqual(false, img.complete);
    img.onload = function(){
      ++n;
      assert.equal(img.src, png);
    };

    img.onerror = function () {
      asset.fail('called onerror');
    }

    img.src = png;
    assert.equal(img.src, png);
    assert.strictEqual(true, img.complete);
    assert.strictEqual(320, img.width);
    assert.strictEqual(320, img.height);
    assert.equal(1, n);
  },
  
  'test Image#onerror': function(){
    var img = new Image
      , error
      , n = 0;

    assert.strictEqual(false, img.complete);
    img.onload = function(){
      assert.fail('called onload');
    };
    
    img.onerror = function(err){
      ++n;
      error = err;
    };

    try {
      img.src = png + 's';
    } catch (err) {
      assert.fail('error did not invoke onerror(): ' + err);
    }

    assert.equal(img.src, png + 's');

    assert.ok(error instanceof Error, 'did not invoke onerror() with error');
    assert.strictEqual(false, img.complete);
    assert.equal(1, n);
  },

  'test Image with cmyk jpeg': function () {
    var img = new Image
      , n = 0;

    assert.strictEqual(false, img.complete);
    img.onload = function() {
      ++n;
    }

    img.onerror = function() {
      assert.fail('called onerror');
    }

    try {
      img.src = cmyk_jpeg;
    } catch (err) {
      assert.fail('threw exception');
    }

    assert.equal(img.src, cmyk_jpeg);
    assert.strictEqual(true, img.complete);
    assert.strictEqual(190, img.width);
    assert.strictEqual(45, img.height);
    assert.equal(1, n);
  },

  'test Image with corrupt jpeg': function () {
    var img = new Image
      , error
      , n = 0;

    assert.strictEqual(false, img.complete);
    img.onload = function(){
      assert.fail('called onload');
    };
    
    img.onerror = function(err){
      ++n;
      error = err;
    };

    try {
      img.src = corrupt_jpeg;
    } catch (err) {
      assert.fail('error did not invoke onerror(): ' + err);
    }

    assert.equal(img.src, corrupt_jpeg);

    assert.ok(error instanceof Error, 'did not invoke onerror() with error');
    assert.strictEqual(false, img.complete);
    assert.equal(1, n);
  },

  'test Image with troublesome png': function () {
    var img = new Image
      , n = 0;

    assert.strictEqual(false, img.complete);
    img.onload = function() {
      ++n;
    }

    img.onerror = function() {
      assert.fail('called onerror');
    }

    try {
      img.src = oom_png;
    } catch (err) {
      assert.fail('threw exception');
    }

    assert.equal(img.src, oom_png);
    assert.strictEqual(true, img.complete);
    assert.strictEqual(35, img.width);
    assert.strictEqual(37, img.height);
    assert.equal(1, n);
  },
  
  'test Image#{width,height}': function(){
    var img = new Image
      , n = 0;
    
    assert.strictEqual(0, img.width);
    assert.strictEqual(0, img.height);
    img.onload = function(){
      ++n;
      assert.strictEqual(320, img.width);
      assert.strictEqual(320, img.height);
    };
    img.src = png;

    assert.equal(1, n);
  }
};
