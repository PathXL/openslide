/*
 *  OpenSlide, a library for reading whole slide image files
 *
 *  Copyright (c) 2007-2012 Carnegie Mellon University
 *  Copyright (c) 2011 Google, Inc.
 *  All rights reserved.
 *
 *  OpenSlide is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU Lesser General Public License as
 *  published by the Free Software Foundation, version 2.1.
 *
 *  OpenSlide is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 *  GNU Lesser General Public License for more details.
 *
 *  You should have received a copy of the GNU Lesser General Public
 *  License along with OpenSlide. If not, see
 *  <http://www.gnu.org/licenses/>.
 *
 */

#include <config.h>

#include "openslide-private.h"

#include <glib.h>
#include <tiffio.h>

#include <stdio.h>
#include <string.h>
#include <math.h>
#include <cairo.h>

#include "openslide-cache.h"
#include "openslide-hash.h"

#define HANDLE_CACHE_MAX 32

struct _openslide_tiffopsdata {
  GQueue *handle_cache;
  GMutex *handle_cache_mutex;
  char *filename;

  _openslide_tiff_tilereader_fn tileread;
};

struct tiff_level {
  struct _openslide_level base;
  struct _openslide_grid *grid;

  tdir_t dir;

  // the world according to TIFF tags
  int64_t raw_width;
  int64_t raw_height;
  int64_t tile_width;
  int64_t tile_height;

  int64_t tiles_across;
  int64_t tiles_down;
  int32_t overlap_x;
  int32_t overlap_y;
};

// not thread-safe, like libtiff
struct tiff_file_handle {
  char *filename;
  int64_t offset;
  int64_t size;
};

struct tiff_associated_image_ctx {
  tdir_t directory;
};

#define SET_DIR_OR_FAIL(osr, tiff, i)				\
  if (!TIFFSetDirectory(tiff, i)) {				\
    _openslide_set_error(osr, "Cannot set TIFF directory");	\
    return;							\
  }

#define GET_FIELD_OR_FAIL(osr, tiff, tag, result)		\
  if (!TIFFGetField(tiff, tag, &tmp)) {				\
    _openslide_set_error(osr, "Cannot get required TIFF tag: %d", tag);	\
    return;							\
  }								\
  result = tmp;

static void read_tile(openslide_t *osr,
		      cairo_t *cr,
		      struct _openslide_level *level,
		      struct _openslide_grid *grid,
		      int64_t tile_x, int64_t tile_y,
		      void *arg);

static const char *store_string_property(TIFF *tiff, GHashTable *ht,
					 const char *name, ttag_t tag) {
  char *value;
  if (TIFFGetFieldDefaulted(tiff, tag, &value)) {
    value = g_strdup(value);
    g_hash_table_insert(ht, g_strdup(name), value);
    return value;
  }
  return NULL;
}

static void store_and_hash_string_property(TIFF *tiff, GHashTable *ht,
					   struct _openslide_hash *quickhash1,
					   const char *name, ttag_t tag) {
  _openslide_hash_string(quickhash1, name);
  _openslide_hash_string(quickhash1, store_string_property(tiff, ht, name, tag));
}

static void store_float_property(TIFF *tiff, GHashTable *ht,
				 const char *name, ttag_t tag) {
  float value;
  if (TIFFGetFieldDefaulted(tiff, tag, &value)) {
    g_hash_table_insert(ht, g_strdup(name), _openslide_format_double(value));
  }
}

static void store_and_hash_properties(TIFF *tiff, GHashTable *ht,
				      struct _openslide_hash *quickhash1) {
  // strings
  store_string_property(tiff, ht, OPENSLIDE_PROPERTY_NAME_COMMENT,
			TIFFTAG_IMAGEDESCRIPTION);

  // strings to store and hash
  store_and_hash_string_property(tiff, ht, quickhash1,
				 "tiff.ImageDescription", TIFFTAG_IMAGEDESCRIPTION);
  store_and_hash_string_property(tiff, ht, quickhash1,
				 "tiff.Make", TIFFTAG_MAKE);
  store_and_hash_string_property(tiff, ht, quickhash1,
				 "tiff.Model", TIFFTAG_MODEL);
  store_and_hash_string_property(tiff, ht, quickhash1,
				 "tiff.Software", TIFFTAG_SOFTWARE);
  store_and_hash_string_property(tiff, ht, quickhash1,
				 "tiff.DateTime", TIFFTAG_DATETIME);
  store_and_hash_string_property(tiff, ht, quickhash1,
				 "tiff.Artist", TIFFTAG_ARTIST);
  store_and_hash_string_property(tiff, ht, quickhash1,
				 "tiff.HostComputer", TIFFTAG_HOSTCOMPUTER);
  store_and_hash_string_property(tiff, ht, quickhash1,
				 "tiff.Copyright", TIFFTAG_COPYRIGHT);
  store_and_hash_string_property(tiff, ht, quickhash1,
				 "tiff.DocumentName", TIFFTAG_DOCUMENTNAME);


  // don't hash floats, they might be unstable over time
  store_float_property(tiff, ht, "tiff.XResolution", TIFFTAG_XRESOLUTION);
  store_float_property(tiff, ht, "tiff.YResolution", TIFFTAG_YRESOLUTION);
  store_float_property(tiff, ht, "tiff.XPosition", TIFFTAG_XPOSITION);
  store_float_property(tiff, ht, "tiff.YPosition", TIFFTAG_YPOSITION);

  // special
  uint16_t resolution_unit;
  if (TIFFGetFieldDefaulted(tiff, TIFFTAG_RESOLUTIONUNIT, &resolution_unit)) {
    const char *result;

    switch(resolution_unit) {
    case 1:
      result = "none";
      break;
    case 2:
      result = "inch";
      break;
    case 3:
      result = "centimeter";
      break;
    default:
      result = "unknown";
    }

    g_hash_table_insert(ht, g_strdup("tiff.ResolutionUnit"), g_strdup(result));
  }
}

static TIFF *get_tiff(openslide_t *osr) {
  struct _openslide_tiffopsdata *data = osr->data;
  TIFF *tiff;

  //g_debug("get TIFF");
  g_mutex_lock(data->handle_cache_mutex);
  tiff = g_queue_pop_head(data->handle_cache);
  g_mutex_unlock(data->handle_cache_mutex);

  if (tiff == NULL) {
    //g_debug("create TIFF");
    // Does not check that we have the same file.  Then again, neither does
    // tiff_do_read.
    tiff = _openslide_tiff_open(data->filename);
  }
  if (tiff == NULL) {
    _openslide_set_error(osr, "Cannot open TIFF file");
  }
  return tiff;
}

static void put_tiff(openslide_t *osr, TIFF *tiff) {
  struct _openslide_tiffopsdata *data = osr->data;

  if (tiff == NULL) {
    return;
  }

  //g_debug("put TIFF");
  g_mutex_lock(data->handle_cache_mutex);
  if (g_queue_get_length(data->handle_cache) < HANDLE_CACHE_MAX) {
    g_queue_push_head(data->handle_cache, tiff);
    tiff = NULL;
  }
  g_mutex_unlock(data->handle_cache_mutex);

  if (tiff) {
    //g_debug("too many TIFFs");
    TIFFClose(tiff);
  }
}

static void destroy_data(struct _openslide_tiffopsdata *data,
                         struct tiff_level **levels, int32_t level_count) {
  TIFF *tiff;

  g_mutex_lock(data->handle_cache_mutex);
  while ((tiff = g_queue_pop_head(data->handle_cache)) != NULL) {
    TIFFClose(tiff);
  }
  g_mutex_unlock(data->handle_cache_mutex);
  g_queue_free(data->handle_cache);
  g_mutex_free(data->handle_cache_mutex);
  g_free(data->filename);
  g_slice_free(struct _openslide_tiffopsdata, data);

  for (int32_t i = 0; i < level_count; i++) {
    _openslide_grid_destroy(levels[i]->grid);
    g_slice_free(struct tiff_level, levels[i]);
  }
  g_free(levels);
}

static void destroy(openslide_t *osr) {
  struct _openslide_tiffopsdata *data = osr->data;
  struct tiff_level **levels = (struct tiff_level **) osr->levels;
  destroy_data(data, levels, osr->level_count);
}


static void set_dimensions(openslide_t *osr, TIFF *tiff,
                           struct tiff_level *l, bool geometry) {
  uint32_t tmp;

  // set the directory
  SET_DIR_OR_FAIL(osr, tiff, l->dir)

  // figure out tile size
  int64_t tw, th;
  GET_FIELD_OR_FAIL(osr, tiff, TIFFTAG_TILEWIDTH, tw)
  GET_FIELD_OR_FAIL(osr, tiff, TIFFTAG_TILELENGTH, th)

  // get image size
  int64_t iw, ih;
  GET_FIELD_OR_FAIL(osr, tiff, TIFFTAG_IMAGEWIDTH, iw)
  GET_FIELD_OR_FAIL(osr, tiff, TIFFTAG_IMAGELENGTH, ih)

  // safe now, start writing
  l->raw_width = iw;
  l->raw_height = ih;
  l->tile_width = tw;
  l->tile_height = th;
  if (geometry) {
    l->base.tile_w = tw;
    l->base.tile_h = th;
  }

  // num tiles in each dimension
  l->tiles_across = (iw / tw) + !!(iw % tw);   // integer ceiling
  l->tiles_down = (ih / th) + !!(ih % th);

  // subtract out the overlaps (there are tiles-1 overlaps in each dimension)
  l->base.w = iw;
  l->base.h = ih;
  if (iw >= tw) {
    l->base.w -= (l->tiles_across - 1) * l->overlap_x;
  }
  if (ih >= th) {
    l->base.h -= (l->tiles_down - 1) * l->overlap_y;
  }

  // set up grid
  l->grid = _openslide_grid_create_simple(osr,
                                          l->tiles_across,
                                          l->tiles_down,
                                          l->tile_width - l->overlap_x,
                                          l->tile_height - l->overlap_y,
                                          read_tile);
}

static void read_tile(openslide_t *osr,
		      cairo_t *cr,
		      struct _openslide_level *level,
		      struct _openslide_grid *grid,
		      int64_t tile_x, int64_t tile_y,
		      void *arg) {
  struct _openslide_tiffopsdata *data = osr->data;
  struct tiff_level *l = (struct tiff_level *) level;
  TIFF *tiff = arg;

  // set the directory
  SET_DIR_OR_FAIL(osr, tiff, l->dir)

  // tile size
  int64_t tw = l->tile_width;
  int64_t th = l->tile_height;

  // image size
  int64_t iw = l->raw_width;
  int64_t ih = l->raw_height;

  int64_t x = tile_x * tw;
  int64_t y = tile_y * th;

  // cache
  struct _openslide_cache_entry *cache_entry;
  uint32_t *tiledata = _openslide_cache_get(osr->cache, x, y, level,
                                            &cache_entry);
  if (!tiledata) {
    tiledata = g_slice_alloc(tw * th * 4);
    data->tileread(osr, tiff, tiledata, x, y, tw, th);

    // clip, if necessary
    int64_t rx = iw - x;
    int64_t ry = ih - y;
    if ((rx < tw) || (ry < th)) {
      cairo_surface_t *surface = cairo_image_surface_create_for_data((unsigned char *) tiledata,
								     CAIRO_FORMAT_ARGB32,
								     tw, th,
								     tw * 4);
      cairo_t *cr2 = cairo_create(surface);
      cairo_surface_destroy(surface);

      cairo_set_operator(cr2, CAIRO_OPERATOR_CLEAR);

      cairo_rectangle(cr2, rx, 0, tw - rx, th);
      cairo_fill(cr2);

      cairo_rectangle(cr2, 0, ry, tw, th - ry);
      cairo_fill(cr2);

      _openslide_check_cairo_status_possibly_set_error(osr, cr2);
      cairo_destroy(cr2);
    }

    // put it in the cache
    _openslide_cache_put(osr->cache, x, y, level,
			 tiledata, tw * th * 4,
			 &cache_entry);
  }

  // draw it
  cairo_surface_t *surface = cairo_image_surface_create_for_data((unsigned char *) tiledata,
								 CAIRO_FORMAT_ARGB32,
								 tw, th,
								 tw * 4);
  cairo_set_source_surface(cr, surface, 0, 0);
  cairo_surface_destroy(surface);
  cairo_paint(cr);

  /*
  cairo_save(cr);
  int z = 4;
  char *zz = g_strdup_printf("%" G_GINT64_FORMAT ",%" G_GINT64_FORMAT " (%" G_GINT64_FORMAT ")",
			     tile_x, tile_y, tile_y * l->tiles_across + tile_x);
  cairo_set_source_rgb(cr, 0, 0, 0);
  cairo_move_to(cr, 0, 20);
  cairo_show_text(cr, zz);
  cairo_set_source_rgba(cr, 1.0, 0, 0, 0.2);
  cairo_rectangle(cr, 0, 0, z, z);
  cairo_fill(cr);
  cairo_rectangle(cr, tw-z, 0, z, z);
  cairo_fill(cr);
  cairo_rectangle(cr, 0, th-z, z, z);
  cairo_fill(cr);
  cairo_rectangle(cr, tw-z, th-z, z, z);
  cairo_fill(cr);
  g_free(zz);
  cairo_restore(cr);
  */


  // done with the cache entry, release it
  _openslide_cache_entry_unref(cache_entry);
}

static void _paint_region(openslide_t *osr, TIFF *tiff, cairo_t *cr,
                          int64_t x, int64_t y,
                          struct _openslide_level *level,
                          int32_t w, int32_t h) {
  struct tiff_level *l = (struct tiff_level *) level;

  // set the directory
  SET_DIR_OR_FAIL(osr, tiff, l->dir)

  _openslide_grid_paint_region(l->grid, cr, tiff,
                               x / level->downsample,
                               y / level->downsample,
                               level, w, h);
}

static void paint_region(openslide_t *osr, cairo_t *cr,
			 int64_t x, int64_t y,
			 struct _openslide_level *level,
			 int32_t w, int32_t h) {
  TIFF *tiff = get_tiff(osr);
  if (tiff) {
    _paint_region(osr, tiff, cr, x, y, level, w, h);
  }
  put_tiff(osr, tiff);
}


static const struct _openslide_ops _openslide_tiff_ops = {
  .paint_region = paint_region,
  .destroy = destroy,
};

void _openslide_add_tiff_ops(openslide_t *osr,
			     TIFF *tiff,
			     int32_t property_dir,
			     int32_t overlap_count,
			     int32_t *overlaps,
			     int32_t level_count,
			     int32_t *directories,
			     _openslide_tiff_tilereader_fn tileread,
			     struct _openslide_hash *quickhash1) {
  // allocate private data
  struct _openslide_tiffopsdata *data =
    g_slice_new(struct _openslide_tiffopsdata);

  GError *tmp_err = NULL;

  // create levels
  struct tiff_level **levels = g_new(struct tiff_level *, level_count);
  for (int32_t i = 0; i < level_count; i++) {
    struct tiff_level *l = g_slice_new0(struct tiff_level);
    l->dir = directories[i];
    if (i < overlap_count) {
      l->overlap_x = overlaps[2 * i];
      l->overlap_y = overlaps[2 * i + 1];
    }
    levels[i] = l;
  }
  g_free(directories);
  g_free(overlaps);

  // populate private data
  data->handle_cache = g_queue_new();
  data->handle_cache_mutex = g_mutex_new();
  data->filename = g_strdup(TIFFFileName(tiff));
  data->tileread = tileread;

  if (osr == NULL) {
    // free now and return
    TIFFClose(tiff);
    destroy_data(data, levels, level_count);
    return;
  }

  // if any level has overlaps, reporting tile advances would mislead the
  // application
  bool report_geometry = true;
  for (int32_t i = 0; i < level_count; i++) {
    if (levels[i]->overlap_x || levels[i]->overlap_y) {
      report_geometry = false;
      break;
    }
  }

  // set dimensions
  for (int32_t i = 0; i < level_count; i++) {
    set_dimensions(osr, tiff, levels[i], report_geometry);
  }

  // generate hash of the smallest level
  TIFFSetDirectory(tiff, levels[level_count - 1]->dir);
  if (!_openslide_hash_tiff_tiles(quickhash1, tiff, &tmp_err)) {
    _openslide_set_error(osr, "Cannot hash TIFF tiles: %s", tmp_err->message);
    g_clear_error(&tmp_err);
  }

  // load TIFF properties
  TIFFSetDirectory(tiff, property_dir);    // ignoring return value, but nothing we can do if failed
  store_and_hash_properties(tiff, osr->properties, quickhash1);

  // store tiff-specific data into osr
  g_assert(osr->data == NULL);
  g_assert(osr->levels == NULL);

  // general osr data
  osr->levels = (struct _openslide_level **) levels;
  osr->level_count = level_count;
  osr->data = data;
  osr->ops = &_openslide_tiff_ops;

  // now store TIFF handle
  put_tiff(osr, tiff);
}

void _openslide_generic_tiff_tilereader(openslide_t *osr,
					TIFF *tiff,
					uint32_t *dest,
					int64_t x, int64_t y,
					int32_t w, int32_t h) {
  TIFFRGBAImage img;
  char emsg[1024] = "";

  // init
  if (!TIFFRGBAImageOK(tiff, emsg)) {
    _openslide_set_error(osr, "Failure in TIFFRGBAImageOK: %s", emsg);
    return;
  }
  if (!TIFFRGBAImageBegin(&img, tiff, 1, emsg)) {
    _openslide_set_error(osr, "Failure in TIFFRGBAImageBegin: %s", emsg);
    return;
  }
  img.req_orientation = ORIENTATION_TOPLEFT;
  img.col_offset = x;
  img.row_offset = y;

  // draw it
  if (TIFFRGBAImageGet(&img, dest, w, h)) {
    // permute
    uint32_t *p = dest;
    uint32_t *end = dest + w * h;
    while (p < end) {
      uint32_t val = *p;
      *p++ = (val & 0xFF00FF00)
	| ((val << 16) & 0xFF0000)
	| ((val >> 16) & 0xFF);
    }
  } else {
    _openslide_set_error(osr, "TIFFRGBAImageGet failed: %s", emsg);
    memset(dest, 0, w * h * 4);
  }

  // done
  TIFFRGBAImageEnd(&img);
}

static void _tiff_get_associated_image_data(openslide_t *osr, TIFF *tiff,
                                            void *_ctx,
                                            uint32_t *dest,
                                            int64_t w, int64_t h) {
  struct tiff_associated_image_ctx *ctx = _ctx;
  uint32_t tmp;
  int64_t width, height;

  // g_debug("read TIFF associated image: %d", ctx->directory);

  SET_DIR_OR_FAIL(osr, tiff, ctx->directory);

  // ensure dimensions have not changed
  GET_FIELD_OR_FAIL(osr, tiff, TIFFTAG_IMAGEWIDTH, width);
  GET_FIELD_OR_FAIL(osr, tiff, TIFFTAG_IMAGELENGTH, height);
  if (w != width || h != height) {
    _openslide_set_error(osr, "Unexpected associated image size");
    return;
  }

  // load the image
  _openslide_generic_tiff_tilereader(osr, tiff, dest, 0, 0, w, h);
}

static void tiff_get_associated_image_data(openslide_t *osr, void *ctx,
                                           uint32_t *dest,
                                           int64_t w, int64_t h)
{
  TIFF *tiff = get_tiff(osr);
  if (tiff) {
    _tiff_get_associated_image_data(osr, tiff, ctx, dest, w, h);
  }
  put_tiff(osr, tiff);
}

static void tiff_destroy_associated_image_ctx(void *_ctx) {
  struct tiff_associated_image_ctx *ctx = _ctx;

  g_slice_free(struct tiff_associated_image_ctx, ctx);
}

bool _openslide_add_tiff_associated_image(GHashTable *ht,
					  const char *name,
					  TIFF *tiff,
					  GError **err) {
  uint32_t tmp;

  // get the dimensions
  if (!TIFFGetField(tiff, TIFFTAG_IMAGEWIDTH, &tmp)) {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_BAD_DATA,
                "Cannot get associated image width");
    return false;
  }
  int64_t w = tmp;

  if (!TIFFGetField(tiff, TIFFTAG_IMAGELENGTH, &tmp)) {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_BAD_DATA,
                "Cannot get associated image height");
    return false;
  }
  int64_t h = tmp;

  // possibly load into struct
  if (ht) {
    struct tiff_associated_image_ctx *ctx =
      g_slice_new(struct tiff_associated_image_ctx);
    ctx->directory = TIFFCurrentDirectory(tiff);

    struct _openslide_associated_image *aimg =
      g_slice_new(struct _openslide_associated_image);
    aimg->w = w;
    aimg->h = h;
    aimg->ctx = ctx;
    aimg->get_argb_data = tiff_get_associated_image_data;
    aimg->destroy_ctx = tiff_destroy_associated_image_ctx;

    // save
    g_hash_table_insert(ht, g_strdup(name), aimg);
  }

  return true;
}

static tsize_t tiff_do_read(thandle_t th, tdata_t buf, tsize_t size) {
  struct tiff_file_handle *hdl = th;

  // don't leave the file handle open between calls
  // also ensures FD_CLOEXEC is set
  FILE *f = _openslide_fopen(hdl->filename, "rb", NULL);
  if (f == NULL) {
    return 0;
  }
  if (fseeko(f, hdl->offset, SEEK_SET)) {
    fclose(f);
    return 0;
  }
  int64_t rsize = fread(buf, 1, size, f);
  hdl->offset += rsize;
  fclose(f);
  return rsize;
}

static tsize_t tiff_do_write(thandle_t th G_GNUC_UNUSED,
                             tdata_t data G_GNUC_UNUSED,
                             tsize_t size G_GNUC_UNUSED) {
  // fail
  return 0;
}

static toff_t tiff_do_seek(thandle_t th, toff_t offset, int whence) {
  struct tiff_file_handle *hdl = th;

  switch (whence) {
  case SEEK_SET:
    hdl->offset = offset;
    break;
  case SEEK_CUR:
    hdl->offset += offset;
    break;
  case SEEK_END:
    hdl->offset = hdl->size + offset;
    break;
  default:
    g_assert_not_reached();
  }
  return hdl->offset;
}

static int tiff_do_close(thandle_t th) {
  struct tiff_file_handle *hdl = th;

  g_free(hdl->filename);
  g_slice_free(struct tiff_file_handle, hdl);
  return 0;
}

static toff_t tiff_do_size(thandle_t th) {
  struct tiff_file_handle *hdl = th;

  return hdl->size;
}

TIFF *_openslide_tiff_open(const char *filename) {
  // open
  FILE *f = _openslide_fopen(filename, "rb", NULL);
  if (f == NULL) {
    return NULL;
  }

  // read magic
  uint8_t buf[4];
  if (fread(buf, 4, 1, f) != 1) {
    // can't read
    fclose(f);
    return NULL;
  }

  // get size
  if (fseeko(f, 0, SEEK_END) == -1) {
    fclose(f);
    return NULL;
  }
  int64_t size = ftello(f);
  fclose(f);
  if (size == -1) {
    return NULL;
  }

  // check magic
  // TODO: remove if libtiff gets private error/warning callbacks
  if (buf[0] != buf[1]) {
    return NULL;
  }
  uint16_t version;
  switch (buf[0]) {
  case 'M':
    // big endian
    version = (buf[2] << 8) | buf[3];
    break;
  case 'I':
    // little endian
    version = (buf[3] << 8) | buf[2];
    break;
  default:
    return NULL;
  }
  // only accept BigTIFF on libtiff >= 4
  if (!(version == 42 || (sizeof(toff_t) > 4 && version == 43))) {
    return NULL;
  }

  // allocate
  struct tiff_file_handle *hdl = g_slice_new0(struct tiff_file_handle);
  hdl->filename = g_strdup(filename);
  hdl->size = size;

  // TIFFOpen
  // mode: m disables mmap to avoid sigbus and other mmap fragility
  TIFF *tiff = TIFFClientOpen(filename, "rm", hdl,
                              tiff_do_read, tiff_do_write, tiff_do_seek,
                              tiff_do_close, tiff_do_size, NULL, NULL);
  if (tiff == NULL) {
    tiff_do_close(hdl);
  }
  return tiff;
}
