
//
// Image.cc
//
// Copyright (c) 2010 LearnBoost <tj@learnboost.com>
//

#include "Canvas.h"
#include "Image.h"
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <node_buffer.h>

#ifdef HAVE_GIF
#include <gif_lib.h>
typedef struct {
  uint8_t *buf;
  unsigned len;
  unsigned pos;
} gif_data_t;
#endif

Persistent<FunctionTemplate> Image::constructor;

/*
 * Read closure used by loadFromBuffer.
 */

typedef struct {
  unsigned len;
  uint8_t *buf;
} read_closure_t;

/*
 * Setup error handler struct for gracefully dealing with jpeg errors.
 */

typedef struct {
  struct jpeg_error_mgr pub;
  jmp_buf setjmp_buffer;
} jpeg_error_manager;

/*
 * JPEG error handler for jumping back.
 */

void
handle_jpeg_error (j_common_ptr cinfo) {
  longjmp(((jpeg_error_manager *) cinfo->err)->setjmp_buffer, 1);
}

/*
 * Helper methods for creating / disposing a JPEG decompressor.
 */

jpeg_decompress_struct*
create_jpeg_decompressor() {
  struct jpeg_decompress_struct *info = (jpeg_decompress_struct *) malloc(sizeof(jpeg_decompress_struct));
  jpeg_error_manager *err = (jpeg_error_manager *) malloc(sizeof(jpeg_error_manager));
  info->err = jpeg_std_error((jpeg_error_mgr *) err);
  err->pub.error_exit = handle_jpeg_error;
  jpeg_create_decompress(info);
  return info;
}

void
dispose_jpeg_decompressor(jpeg_decompress_struct *info) {
  jpeg_destroy_decompress(info);
  free(info->err);
  free(info);
}

/*
 * Initialize Image.
 */

void
Image::Initialize(Handle<Object> target) {
  HandleScope scope;

  // Constructor
  constructor = Persistent<FunctionTemplate>::New(FunctionTemplate::New(Image::New));
  constructor->InstanceTemplate()->SetInternalFieldCount(1);
  constructor->SetClassName(String::NewSymbol("Image"));

  // Prototype
  Local<ObjectTemplate> proto = constructor->PrototypeTemplate();
  proto->SetAccessor(String::NewSymbol("source"), GetSource, SetSource);
  proto->SetAccessor(String::NewSymbol("complete"), GetComplete);
  proto->SetAccessor(String::NewSymbol("width"), GetWidth);
  proto->SetAccessor(String::NewSymbol("height"), GetHeight);
  proto->SetAccessor(String::NewSymbol("onload"), GetOnload, SetOnload);
  proto->SetAccessor(String::NewSymbol("onerror"), GetOnerror, SetOnerror);
#if CAIRO_VERSION_MINOR >= 10
  proto->SetAccessor(String::NewSymbol("dataMode"), GetDataMode, SetDataMode);
  constructor->Set(String::NewSymbol("MODE_IMAGE"), Number::New(1));
  constructor->Set(String::NewSymbol("MODE_MIME"), Number::New(2));
#endif
  target->Set(String::NewSymbol("Image"), constructor->GetFunction());
}

/*
 * Initialize a new Image.
 */

Handle<Value>
Image::New(const Arguments &args) {
  HandleScope scope;
  Image *img = new Image;
  img->data_mode = DATA_IMAGE;
  img->Wrap(args.This());
  return args.This();
}

/*
 * Get complete boolean.
 */

Handle<Value>
Image::GetComplete(Local<String>, const AccessorInfo &info) {
  HandleScope scope;
  Image *img = ObjectWrap::Unwrap<Image>(info.This());
  return scope.Close(Boolean::New(Image::COMPLETE == img->state));
}

#if CAIRO_VERSION_MINOR >= 10

/*
 * Get dataMode.
 */

Handle<Value>
Image::GetDataMode(Local<String>, const AccessorInfo &info) {
  HandleScope scope;
  Image *img = ObjectWrap::Unwrap<Image>(info.This());
  return scope.Close(Number::New(img->data_mode));
}

/*
 * Set dataMode.
 */

void
Image::SetDataMode(Local<String>, Local<Value> val, const AccessorInfo &info) {
  if (val->IsNumber()) {
    Image *img = ObjectWrap::Unwrap<Image>(info.This());
    int mode = val->Uint32Value();
    switch (mode) {
      case 1:
        img->data_mode = DATA_IMAGE;
        break;
      case 2:
        img->data_mode = DATA_MIME;
        break;
      case 3:
        img->data_mode = DATA_IMAGE_AND_MIME;
        break;
    }
  }
}

#endif

/*
 * Get width.
 */

Handle<Value>
Image::GetWidth(Local<String>, const AccessorInfo &info) {
  HandleScope scope;
  Image *img = ObjectWrap::Unwrap<Image>(info.This());
  return scope.Close(Number::New(img->width));
}
/*
 * Get height.
 */

Handle<Value>
Image::GetHeight(Local<String>, const AccessorInfo &info) {
  HandleScope scope;
  Image *img = ObjectWrap::Unwrap<Image>(info.This());
  return scope.Close(Number::New(img->height));
}

/*
 * Get src path.
 */

Handle<Value>
Image::GetSource(Local<String>, const AccessorInfo &info) {
  HandleScope scope;
  Image *img = ObjectWrap::Unwrap<Image>(info.This());
  return scope.Close(String::New(img->filename ? img->filename : ""));
}

/*
 * Clean up assets and variables.
 */

void
Image::clearData() {
  if (_surface) {
    cairo_surface_destroy(_surface);
    V8::AdjustAmountOfExternalAllocatedMemory(-_data_len);
    _data_len = 0;
    _surface = NULL;
  }

  free(_data);
  _data = NULL;

  width = height = 0;

  free(filename);
  filename = NULL;

  state = DEFAULT;
}

/*
 * Set src path.
 */

void
Image::SetSource(Local<String>, Local<Value> val, const AccessorInfo &info) {
  HandleScope scope;
  Image *img = ObjectWrap::Unwrap<Image>(info.This());
  cairo_status_t status = CAIRO_STATUS_READ_ERROR;

  img->clearData();

  // url string
  if (val->IsString()) {
    String::AsciiValue src(val);
    if (img->filename) free(img->filename);
    img->filename = strdup(*src);
    status = img->load();
  // Buffer
  } else if (Buffer::HasInstance(val)) {
    uint8_t *buf = (uint8_t *) Buffer::Data(val->ToObject());
    unsigned len = Buffer::Length(val->ToObject());
    status = img->loadFromBuffer(buf, len);
  }

  // check status
  if (status) {
    img->error(Canvas::Error(status));
  } else {
    img->loaded();
  }
}

/*
 * Load image data from `buf` by sniffing
 * the bytes to determine format.
 */

cairo_status_t
Image::loadFromBuffer(uint8_t *buf, unsigned len) {
  if (isPNG(buf)) return loadPNGFromBuffer(buf);
#ifdef HAVE_GIF
  if (isGIF(buf)) return loadGIFFromBuffer(buf, len);
#endif
#ifdef HAVE_JPEG
#if CAIRO_VERSION_MINOR < 10
  if (isJPEG(buf)) return loadJPEGFromBuffer(buf, len);    
#else
  if (isJPEG(buf)) {
    switch (data_mode) {
      case DATA_IMAGE:
        return loadJPEGFromBuffer(buf, len);
      case DATA_MIME:
        return decodeJPEGBufferIntoMimeSurface(buf, len);
      case DATA_IMAGE_AND_MIME:
        cairo_status_t status;
        status = loadJPEGFromBuffer(buf, len);
        if (status) return status;
        return assignDataAsMime(buf, len, CAIRO_MIME_TYPE_JPEG);
    }
  }
#endif
#endif
  return CAIRO_STATUS_READ_ERROR;
}

/*
 * Load PNG data from `buf`.
 */

cairo_status_t
Image::loadPNGFromBuffer(uint8_t *buf) {
  read_closure_t closure;
  closure.len = 0;
  closure.buf = buf;
  _surface = cairo_image_surface_create_from_png_stream(readPNG, &closure);
  cairo_status_t status = cairo_surface_status(_surface);
  if (status) return status;
  return CAIRO_STATUS_SUCCESS;
}

/*
 * Read PNG data.
 */

cairo_status_t
Image::readPNG(void *c, uint8_t *data, unsigned int len) {
  read_closure_t *closure = (read_closure_t *) c;
  memcpy(data, closure->buf + closure->len, len);
  closure->len += len;
  return CAIRO_STATUS_SUCCESS;
}

/*
 * Get onload callback.
 */

Handle<Value>
Image::GetOnload(Local<String>, const AccessorInfo &info) {
  Image *img = ObjectWrap::Unwrap<Image>(info.This());
  return img->onload;
}

/*
 * Set onload callback.
 */

void
Image::SetOnload(Local<String>, Local<Value> val, const AccessorInfo &info) {
  if (val->IsFunction()) {
    Image *img = ObjectWrap::Unwrap<Image>(info.This());
    img->onload = Persistent<Function>::New(Handle<Function>::Cast(val));
  }
}

/*
 * Get onerror callback.
 */

Handle<Value>
Image::GetOnerror(Local<String>, const AccessorInfo &info) {
  Image *img = ObjectWrap::Unwrap<Image>(info.This());
  return img->onerror;
}

/*
 * Set onerror callback.
 */

void
Image::SetOnerror(Local<String>, Local<Value> val, const AccessorInfo &info) {
  if (val->IsFunction()) {
    Image *img = ObjectWrap::Unwrap<Image>(info.This());
    img->onerror = Persistent<Function>::New(Handle<Function>::Cast(val));
  }
}

/*
 * Initialize a new Image.
 */

Image::Image() {
  filename = NULL;
  _data = NULL;
  _data_len = 0;
  _surface = NULL;
  width = height = 0;
  state = DEFAULT;
}

/*
 * Destroy image and associated surface.
 */

Image::~Image() {
  clearData();
}

/*
 * Initiate image loading.
 */

cairo_status_t
Image::load() {
  if (LOADING != state) {
    state = LOADING;
    return loadSurface();
  }
  return CAIRO_STATUS_READ_ERROR;
}

/*
 * Invoke onload (when assigned) and assign dimensions.
 */

void
Image::loaded() {
  HandleScope scope;

  width = cairo_image_surface_get_width(_surface);
  height = cairo_image_surface_get_height(_surface);
  _data_len = height * cairo_image_surface_get_stride(_surface);
  V8::AdjustAmountOfExternalAllocatedMemory(_data_len);

  // At this point we have a valid surface, but may have errored out
  // inside image decoding. If so, don't run the onload callback.
  if (state != INVALID) {
    state = COMPLETE;

    if (!onload.IsEmpty()) {
      TryCatch try_catch;
      onload->Call(Context::GetCurrent()->Global(), 0, NULL);
      onload.Dispose();
      if (try_catch.HasCaught()) {
        error(try_catch.Exception());
      }
    }
  }
}

/*
 * Invoke onerror (when assigned) with the given err.
 */

void
Image::error(Local<Value> err) {
  HandleScope scope;
  if (!onerror.IsEmpty()) {
    Local<Value> argv[1] = { err };
    TryCatch try_catch;
    onerror->Call(Context::GetCurrent()->Global(), 1, argv);
    onerror.Dispose();
    if (try_catch.HasCaught()) {
      FatalException(try_catch);
    }
  }
}

/*
 * Load cairo surface from the image src.
 * 
 * TODO: support more formats
 * TODO: use node IO or at least thread pool
 */

cairo_status_t
Image::loadSurface() {
  FILE *stream = fopen(filename, "r");
  if (!stream) return CAIRO_STATUS_READ_ERROR;
  uint8_t buf[5];
  if (1 != fread(&buf, 5, 1, stream)) return CAIRO_STATUS_READ_ERROR;
  fseek(stream, 0, SEEK_SET);

  // png
  if (isPNG(buf)) {
    fclose(stream);
    return loadPNG();
  }

  // gif
#ifdef HAVE_GIF
  if (isGIF(buf)) return loadGIF(stream);
#endif

  // jpeg
#ifdef HAVE_JPEG
  if (isJPEG(buf)) return loadJPEG(stream);
#endif

  return CAIRO_STATUS_READ_ERROR;
}

/*
 * Load PNG.
 */

cairo_status_t
Image::loadPNG() {
  _surface = cairo_image_surface_create_from_png(filename);
  return cairo_surface_status(_surface);
}

// GIF support

#ifdef HAVE_GIF

/*
 * Return the alpha color for `gif` at `frame`, or -1.
 */

int
get_gif_transparent_color(GifFileType *gif, int frame) {
  ExtensionBlock *ext = gif->SavedImages[frame].ExtensionBlocks;
  int len = gif->SavedImages[frame].ExtensionBlockCount;
  for (int x = 0; x < len; ++x, ++ext) {
    if ((ext->Function == GRAPHICS_EXT_FUNC_CODE) && (ext->Bytes[0] & 1)) {
      return ext->Bytes[3] == 0 ? 0 : (uint8_t) ext->Bytes[3]; 
    }
  }
  return -1;
}

/*
 * Memory GIF reader callback.
 */

int
read_gif_from_memory(GifFileType *gif, GifByteType *buf, int len) {
  gif_data_t *data = (gif_data_t *) gif->UserData;
  if ((data->pos + len) > data->len) len = data->len - data->pos;
  memcpy(buf, data->pos + data->buf, len);
  data->pos += len;
  return len;
}

/*
 * Load GIF.
 */

cairo_status_t
Image::loadGIF(FILE *stream) {
  struct stat s;
  int fd = fileno(stream);

  // stat
  if (fstat(fd, &s) < 0) {
    fclose(stream);
    return CAIRO_STATUS_READ_ERROR;
  } 

  uint8_t *buf = (uint8_t *) malloc(s.st_size);

  if (!buf) {
    fclose(stream);
    return CAIRO_STATUS_NO_MEMORY;
  }

  size_t read = fread(buf, s.st_size, 1, stream);
  fclose(stream);

  cairo_status_t result = CAIRO_STATUS_READ_ERROR;
  if (1 == read) result = loadGIFFromBuffer(buf, s.st_size);
  free(buf);

  return result;
}

/*
 * Load give from `buf` and the given `len`.
 */

cairo_status_t
Image::loadGIFFromBuffer(uint8_t *buf, unsigned len) {
  int i = 0;
  GifFileType* gif;

  gif_data_t gifd = { buf, len, 0 };

  if ((gif = DGifOpen((void*) &gifd, read_gif_from_memory)) == NULL)
    return CAIRO_STATUS_READ_ERROR; 

  if (GIF_OK != DGifSlurp(gif)) {
    DGifCloseFile(gif);
    return CAIRO_STATUS_READ_ERROR;
  }

  width = gif->SWidth;
  height = gif->SHeight;

  uint8_t *data = (uint8_t *) malloc(width * height * 4);
  if (!data) {
    DGifCloseFile(gif);
    return CAIRO_STATUS_NO_MEMORY;
  }

  GifImageDesc *img = &gif->SavedImages[i].ImageDesc;

  // local colormap takes precedence over global
  ColorMapObject *colormap = img->ColorMap
    ? img->ColorMap
    : gif->SColorMap;

  int bgColor = 0;
  int alphaColor = get_gif_transparent_color(gif, i);
  if (gif->SColorMap) bgColor = (uint8_t) gif->SBackGroundColor;
  else if(alphaColor >= 0) bgColor = alphaColor;

  uint8_t *src_data = (uint8_t*) gif->SavedImages[i].RasterBits;
  uint32_t *dst_data = (uint32_t*) data;

  if (!gif->Image.Interlace) {
    if (width == img->Width && height == img->Height) {
      for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
          *dst_data = ((*src_data == alphaColor) ? 0 : 255) << 24
            | colormap->Colors[*src_data].Red << 16
            | colormap->Colors[*src_data].Green << 8
            | colormap->Colors[*src_data].Blue;

          dst_data++;
          src_data++;
        }
      }
    } else {
      // Image does not take up whole "screen" so we need to fill-in the background
      int bottom = img->Top + img->Height;
      int right = img->Left + img->Width;

      for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
          if (y < img->Top || y >= bottom || x < img->Left || x >= right) {
            *dst_data = ((bgColor == alphaColor) ? 0 : 255) << 24
              | colormap->Colors[bgColor].Red << 16
              | colormap->Colors[bgColor].Green << 8
              | colormap->Colors[bgColor].Blue;
          } else {
            *dst_data = ((*src_data == alphaColor) ? 0 : 255) << 24
              | colormap->Colors[*src_data].Red << 16
              | colormap->Colors[*src_data].Green << 8
              | colormap->Colors[*src_data].Blue;
          }

          dst_data++;
          src_data++;
        }
      } 
    }
  } else { 
    // Image is interlaced so that it streams nice over 14.4k and 28.8k modems :)
    // We first load in 1/8 of the image, followed by another 1/8, followed by
    // 1/4 and finally the remaining 1/2.
    int ioffs[] = { 0, 4, 2, 1 };
    int ijumps[] = { 8, 8, 4, 2 };

    uint8_t *src_ptr = src_data;
    uint32_t *dst_ptr;

    for(int z = 0; z < 4; z++) {
      for(int y = ioffs[z]; y < height; y += ijumps[z]) {
        dst_ptr = dst_data + width * y;
        for(int x = 0; x < width; ++x) {
          *dst_ptr = ((*src_ptr == alphaColor) ? 0 : 255) << 24
            | (colormap->Colors[*src_ptr].Red) << 16
            | (colormap->Colors[*src_ptr].Green) << 8
            | (colormap->Colors[*src_ptr].Blue);

          dst_ptr++;
          src_ptr++;
        }
      }
    }
  }

  DGifCloseFile(gif);

  // New image surface
  _surface = cairo_image_surface_create_for_data(
      data
    , CAIRO_FORMAT_ARGB32
    , width
    , height
    , cairo_format_stride_for_width(CAIRO_FORMAT_ARGB32, width));

  cairo_status_t status = cairo_surface_status(_surface);

  if (status) {
    free(data);
    return status;
  }

  _data = data;

  return CAIRO_STATUS_SUCCESS;
}
#endif /* HAVE_GIF */

// JPEG support

#ifdef HAVE_JPEG

// libjpeg 6.2 does not have jpeg_mem_src; define it ourselves here unless
// libjpeg 8 is installed.
#if JPEG_LIB_VERSION < 80

/* Read JPEG image from a memory segment */
static void
init_source(j_decompress_ptr cinfo) {}

static boolean
fill_input_buffer(j_decompress_ptr cinfo) {
  ERREXIT(cinfo, JERR_INPUT_EMPTY);
  return TRUE;
}
static void
skip_input_data(j_decompress_ptr cinfo, long num_bytes) {
  struct jpeg_source_mgr* src = (struct jpeg_source_mgr*) cinfo->src;
  if (num_bytes > 0) {
    src->next_input_byte += (size_t) num_bytes;
    src->bytes_in_buffer -= (size_t) num_bytes;
  }
}

static void term_source (j_decompress_ptr cinfo) {}
static void jpeg_mem_src (j_decompress_ptr cinfo, void* buffer, long nbytes) {
  struct jpeg_source_mgr* src;

  if (cinfo->src == NULL) {
    cinfo->src = (struct jpeg_source_mgr *)
      (*cinfo->mem->alloc_small) ((j_common_ptr) cinfo, JPOOL_PERMANENT,
                                  sizeof(struct jpeg_source_mgr));
  }

  src = (struct jpeg_source_mgr*) cinfo->src;
  src->init_source = init_source;
  src->fill_input_buffer = fill_input_buffer;
  src->skip_input_data = skip_input_data;
  src->resync_to_restart = jpeg_resync_to_restart; /* use default method */
  src->term_source = term_source;
  src->bytes_in_buffer = nbytes;
  src->next_input_byte = (JOCTET*)buffer;
}

#endif

/*
 * Takes an initialised jpeg_decompress_struct and decodes the
 * data into _surface.
 */

cairo_status_t
Image::decodeJPEGIntoSurface(jpeg_decompress_struct *info) {
  cairo_status_t status;
  uint8_t *data = NULL;
  uint8_t *src = NULL;

  // Set JPEG Error Handler
  if (setjmp(((jpeg_error_manager *) info->err)->setjmp_buffer)) {
    error(Exception::Error(String::New(info->err->jpeg_message_table[info->err->msg_code])));
    state = INVALID;
    dispose_jpeg_decompressor(info);
    free(data);
    free(src);
    return createEmptyImageFallback();
  }

  jpeg_read_header(info, TRUE);
  jpeg_start_decompress(info);

  width = info->output_width;
  height = info->output_height;
  int stride = width * 4;
  data = (uint8_t *) malloc(width * height * 4);
  src = (uint8_t *) malloc(width * info->output_components);

  if (!data || !src) {
    free(data);
    free(src);
    dispose_jpeg_decompressor(info);
    return CAIRO_STATUS_NO_MEMORY;
  }

  for (int y = 0; y < height; ++y) {
    jpeg_read_scanlines(info, &src, 1);
    uint32_t *row = (uint32_t *)(data + stride * y);
    for (int x = 0; x < width; ++x) {
      uint8_t r = 0, g = 0, b = 0, k = 0;
      int bx = info->output_components * x;
      switch (info->out_color_space) {
        case JCS_CMYK:
          // TODO: ICC profiles support?
          k = src[bx + 3];
          r = k * src[bx] / 255;
          g = k * src[bx + 1] / 255;
          b = k * src[bx + 2] / 255;
          break;
        case JCS_RGB:
        case JCS_GRAYSCALE:
          if (info->output_components == 1) {
            r = g = b = src[bx];
          } else {
            r = src[bx];
            g = src[bx + 1];
            b = src[bx + 2];
          }
          break;
        case JCS_UNKNOWN:
        case JCS_YCbCr:
        case JCS_YCCK:
          // TODO: handle these other colour space options?
          break;
      }
      uint32_t *pixel = row + x;
      *pixel = 255 << 24
             | r << 16
             | g << 8
             | b;
    }
  }

  dispose_jpeg_decompressor(info);
  free(src);

  _surface = cairo_image_surface_create_for_data(
      data
    , CAIRO_FORMAT_ARGB32
    , width
    , height
    , cairo_format_stride_for_width(CAIRO_FORMAT_ARGB32, width));
  
  status = cairo_surface_status(_surface);

  if (status) {
    free(data);
  } else {
    _data = data;
  }

  return status;
}

#if CAIRO_VERSION_MINOR >= 10

/*
 * Takes a jpeg data buffer and assigns it as mime data to a
 * dummy surface
 */

cairo_status_t
Image::decodeJPEGBufferIntoMimeSurface(uint8_t *buf, unsigned len) {
  uint8_t *data = NULL;
  struct jpeg_decompress_struct *info = create_jpeg_decompressor();

  // Set JPEG Error Handler
  if (setjmp(((jpeg_error_manager *) info->err)->setjmp_buffer)) {
    error(Exception::Error(String::New(info->err->jpeg_message_table[info->err->msg_code])));
    state = INVALID;
    dispose_jpeg_decompressor(info);
    free(data);
    return createEmptyImageFallback();
  }

  jpeg_mem_src(info, buf, len);
  jpeg_read_header(info, 1);
  jpeg_start_decompress(info);

  width = info->output_width;
  height = info->output_height;

  dispose_jpeg_decompressor(info);

  // Data alloc
  // 8 pixels per byte using Alpha Channel format to reduce memory requirement.
  int buf_size = height * cairo_format_stride_for_width(CAIRO_FORMAT_A1, width);
  data = (uint8_t *) malloc(buf_size);
  if (!data) return CAIRO_STATUS_NO_MEMORY;

  // New image surface
  _surface = cairo_image_surface_create_for_data(
      data
    , CAIRO_FORMAT_A1
    , width
    , height
    , cairo_format_stride_for_width(CAIRO_FORMAT_A1, width));

  cairo_status_t status = cairo_surface_status(_surface);

  if (status) {
    free(data);
    return status;
  }

  _data = data;

  return assignDataAsMime(buf, len, CAIRO_MIME_TYPE_JPEG);
}

/*
 * Helper function for disposing of a mime data closure.
 */

void
clearMimeData(void *closure) {
  V8::AdjustAmountOfExternalAllocatedMemory(-((read_closure_t *)closure)->len);
  free(((read_closure_t *) closure)->buf);
  free(closure);
}

/*
 * Assign a given buffer as mime data against the surface.
 * The provided buffer will be copied, and the copy will
 * be automatically freed when the surface is destroyed.
 */

cairo_status_t
Image::assignDataAsMime(uint8_t *data, int len, const char *mime_type) {
  uint8_t *mime_data = (uint8_t *) malloc(len);
  if (!mime_data) return CAIRO_STATUS_NO_MEMORY;

  read_closure_t *mime_closure = (read_closure_t *) malloc(sizeof(read_closure_t));
  if (!mime_closure) {
    free(mime_data);
    return CAIRO_STATUS_NO_MEMORY;
  }

  memcpy(mime_data, data, len);

  mime_closure->buf = mime_data;
  mime_closure->len = len;

  V8::AdjustAmountOfExternalAllocatedMemory(len);

  return cairo_surface_set_mime_data(_surface, mime_type, mime_data, len, clearMimeData, mime_closure);
}

#endif

/*
 * Load jpeg from buffer.
 */

cairo_status_t
Image::loadJPEGFromBuffer(uint8_t *buf, unsigned len) {
  struct jpeg_decompress_struct *info = create_jpeg_decompressor();
  jpeg_mem_src(info, buf, len);
  return decodeJPEGIntoSurface(info);
}

/*
 * On JPEG decode failure can need to create an empty surface
 */

cairo_status_t
Image::createEmptyImageFallback() {
  width = 1;
  height = 1;
  _surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, width, height);
  cairo_t *ctx = cairo_create(_surface);
  cairo_set_source_rgb(ctx, 224.0/256, 27.0/256, 208.0/256);
  cairo_paint(ctx);
  cairo_destroy(ctx);
  return cairo_surface_status(_surface);
}

/*
 * Load JPEG, convert RGB to ARGB.
 */

cairo_status_t
Image::loadJPEG(FILE *stream) {
  cairo_status_t status;

  if (data_mode == DATA_IMAGE) { // Can lazily read in the JPEG.
    struct jpeg_decompress_struct *info = create_jpeg_decompressor();
    jpeg_stdio_src(info, stream);
    status = decodeJPEGIntoSurface(info);
    fclose(stream);
  } else { // We'll need the actual source jpeg data, so read fully.
#if CAIRO_VERSION_MINOR >= 10
    uint8_t *buf;
    unsigned len;

    fseek(stream, 0, SEEK_END);
    len = ftell(stream);
    fseek(stream, 0, SEEK_SET);

    buf = (uint8_t *) malloc(len);
    if (!buf) return CAIRO_STATUS_NO_MEMORY;

    fread(buf, len, 1, stream);
    fclose(stream);

    switch (data_mode) {
      case DATA_IMAGE: // Can't be this, but compiler warning.
      case DATA_IMAGE_AND_MIME:
        status = loadJPEGFromBuffer(buf, len);
        if (status) break;
        status = assignDataAsMime(buf, len, CAIRO_MIME_TYPE_JPEG);
        break;
      case DATA_MIME:
        status = decodeJPEGBufferIntoMimeSurface(buf, len);
        break;
    }

    free(buf);
#endif
  }

  return status;
}

#endif /* HAVE_JPEG */

/*
 * Return UNKNOWN, JPEG, or PNG based on the filename.
 */

Image::type
Image::extension(const char *filename) {
  size_t len = strlen(filename);
  filename += len;
  if (len >= 5 && 0 == strcmp(".jpeg", filename - 5)) return Image::JPEG;
  if (len >= 4 && 0 == strcmp(".gif", filename - 4)) return Image::GIF;
  if (len >= 4 && 0 == strcmp(".jpg", filename - 4)) return Image::JPEG;
  if (len >= 4 && 0 == strcmp(".png", filename - 4)) return Image::PNG;
  return Image::UNKNOWN;
}

/*
 * Sniff bytes for JPEG's magic number ff d8.
 */

int
Image::isJPEG(uint8_t *data) {
  return 0xff == data[0] && 0xd8 == data[1];
}

/*
 * Sniff bytes 0..2 for "GIF".
 */

int
Image::isGIF(uint8_t *data) {
  return 'G' == data[0] && 'I' == data[1] && 'F' == data[2];
}

/*
 * Sniff bytes 1..3 for "PNG".
 */

int
Image::isPNG(uint8_t *data) {
  return 'P' == data[1] && 'N' == data[2] && 'G' == data[3];
}
