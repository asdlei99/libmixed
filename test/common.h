#include <math.h>
#include <mpg123.h>
#include <out123.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "mixed.h"

char volatile interrupted = 0;

void interrupt_handler(int shim){
  interrupted = 1;
}

int fmt123_to_mixed(int fmt){
  switch(fmt){
  case MPG123_ENC_SIGNED_8: return MIXED_INT8;
  case MPG123_ENC_UNSIGNED_8: return MIXED_UINT8;
  case MPG123_ENC_SIGNED_16: return MIXED_INT16;
  case MPG123_ENC_UNSIGNED_16: return MIXED_UINT16;
  case MPG123_ENC_SIGNED_24: return MIXED_INT24;
  case MPG123_ENC_UNSIGNED_24: return MIXED_UINT24;
  case MPG123_ENC_SIGNED_32: return MIXED_INT32;
  case MPG123_ENC_UNSIGNED_32: return MIXED_UINT32;
  case MPG123_ENC_FLOAT_32: return MIXED_FLOAT;
  case MPG123_ENC_FLOAT_64: return MIXED_DOUBLE;
  default: return -1;
  }
}

struct out{
  out123_handle *handle;
  struct mixed_packed_audio pack;
  struct mixed_segment segment;
  struct mixed_buffer left;
  struct mixed_buffer right;
};

void free_out(struct out *out){
  if(!out) return;
  if(out->handle){
    out123_close(out->handle);
    out123_del(out->handle);
  }
  if(out->pack.data){
    free(out->pack.data);
  }
  mixed_free_segment(&out->segment);
  mixed_free_buffer(&out->left);
  mixed_free_buffer(&out->right);
  free(out);
}

int extract_encoding(int encodings){
  if((encodings & MPG123_ENC_FLOAT_32) == MPG123_ENC_FLOAT_32) 
    return MPG123_ENC_FLOAT_32;

  if((encodings & MPG123_ENC_FLOAT_64) == MPG123_ENC_FLOAT_64) 
    return MPG123_ENC_FLOAT_64;

  if((encodings & MPG123_ENC_UNSIGNED_32) == MPG123_ENC_UNSIGNED_32) 
    return MPG123_ENC_UNSIGNED_32;

  if((encodings & MPG123_ENC_SIGNED_32) == MPG123_ENC_SIGNED_32) 
    return MPG123_ENC_SIGNED_32;

  if((encodings & MPG123_ENC_UNSIGNED_24) == MPG123_ENC_UNSIGNED_24) 
    return MPG123_ENC_UNSIGNED_24;

  if((encodings & MPG123_ENC_SIGNED_24) == MPG123_ENC_SIGNED_24) 
    return MPG123_ENC_SIGNED_24;

  if((encodings & MPG123_ENC_UNSIGNED_16) == MPG123_ENC_UNSIGNED_16) 
    return MPG123_ENC_UNSIGNED_16;

  if((encodings & MPG123_ENC_SIGNED_16) == MPG123_ENC_SIGNED_16) 
    return MPG123_ENC_SIGNED_16;

  return -1;
}

int load_out_segment(size_t samples, struct out **_out){
  long out_samplerate = 44100;
  int out_channels = 2;
  int out_encoding = MPG123_ENC_SIGNED_16;
  char *out_encname = "signed 16 bit";
  uint8_t out_samplesize = 0;
  int out_framesize = 0;
  struct out *out = calloc(1, sizeof(struct out));

  if(!out){
    fprintf(stderr, "Failed to allocate mixer data.\n");
    goto cleanup;
  }
  
  out->handle = out123_new();
  if(out->handle == 0){
    fprintf(stderr, "Failed to create OUT123 handle.\n");
    goto cleanup;
  }

  if(out123_open(out->handle, 0, 0) != OUT123_OK){
    fprintf(stderr, "Failed to open sound device: %s\n", out123_strerror(out->handle));
    goto cleanup;
  }

  // Find suitable configuration
  struct mpg123_fmt *fmts;
  long rates[1] = {out_samplerate};
  int count = out123_formats(out->handle, rates, 1, 2, 2, &fmts);
  int fmt_index = 0;

  for(int i=0; i<count; ++i){
    if(fmts[i].rate != -1){
      fmt_index = i;
      const char *encname = out123_enc_longname(fmts[i].encoding);
      fprintf(stderr, "OUT %i: %i channels @ %li Hz, %s\n",
              i, fmts[i].channels, fmts[i].rate, encname);
    }
  }
  
  if(fmt_index == count){
    fprintf(stderr, "No suitable playback format configuration found on the device.\n");
    goto cleanup;
  }

  int encoding = extract_encoding(fmts[fmt_index].encoding);
  if(!encoding){
    fprintf(stderr, "No suitable encoding could be found on the device.\n");
    goto cleanup;
  }

  if(out123_start(out->handle, fmts[fmt_index].rate, fmts[fmt_index].channels, encoding)){
    fprintf(stderr, "Failed to start playback on device: %s\n", out123_strerror(out->handle));
    goto cleanup;
  }
  
  if(out123_getformat(out->handle, &out_samplerate, &out_channels, &out_encoding, &out_framesize) != OUT123_OK){
    fprintf(stderr, "Failed to get format properties of your device: %s\n", out123_strerror(out->handle));
    goto cleanup;
  }

  out_encname = (char *)out123_enc_longname(out_encoding);
  fprintf(stderr, "OUT: %i channels @ %li Hz, %s %i\n", out_channels, out_samplerate, out_encname);
  
  // Prepare pipeline segments
  out->pack.encoding = fmt123_to_mixed(out_encoding);
  out->pack.channels = out_channels;
  out->pack.layout = MIXED_ALTERNATING;
  out->pack.samplerate = out_samplerate;
  out_samplesize = mixed_samplesize(out->pack.encoding);
  out->pack.size = samples*out_samplesize*out_channels;
  out->pack.data = calloc(out->pack.size, sizeof(uint8_t));

  if(!out->pack.data){
    fprintf(stderr, "Couldn't allocate output buffer.\n");
    goto cleanup;
  }
  
  if(!mixed_make_segment_packer(&out->pack, 44100, &out->segment)){
    fprintf(stderr, "Failed to create segments: %s\n", mixed_error_string(-1));
    goto cleanup;
  }

  if(!mixed_make_buffer(samples, &out->left) ||
     !mixed_make_buffer(samples, &out->right)){
    fprintf(stderr, "Failed to allocate mixer buffers: %s\n", mixed_error_string(-1));
    goto cleanup;
  }

  if(!mixed_segment_set_in(MIXED_BUFFER, MIXED_LEFT, &out->left, &out->segment) ||
     !mixed_segment_set_in(MIXED_BUFFER, MIXED_RIGHT, &out->right, &out->segment)){
    fprintf(stderr, "Failed to set buffers for out: %s\n", mixed_error_string(-1));
    goto cleanup;
  }

  *_out = out;
  return 1;

 cleanup:
  free_out(out);
  return 0;
}

struct mp3{
  mpg123_handle *handle;
  struct mixed_packed_audio pack;
  struct mixed_segment segment;
  struct mixed_buffer left;
  struct mixed_buffer right;
};

void free_mp3(struct mp3 *mp3){
  if(!mp3) return;
  if(mp3->handle){
    mpg123_close(mp3->handle);
    mpg123_delete(mp3->handle);
  }
  if(mp3->pack.data){
    free(mp3->pack.data);
  }
  mixed_free_segment(&mp3->segment);
  mixed_free_buffer(&mp3->left);
  mixed_free_buffer(&mp3->right);
  free(mp3);
}

int load_mp3_segment(char *file, size_t samples, struct mp3 **_mp3){
  long mp3_samplerate = 0;
  int mp3_channels = 0;
  int mp3_encoding = 0;
  char *mp3_encname = 0;
  uint8_t mp3_samplesize = 0;
  struct mp3 *mp3 = calloc(1, sizeof(struct mp3));

  if(!mp3){
    fprintf(stderr, "Failed to allocate mixer data.\n");
    goto cleanup;
  }

  mp3->handle = mpg123_new(NULL, 0);
  if(mp3->handle == 0){
    fprintf(stderr, "Failed to create MPG123 handle.\n");
    goto cleanup;
  }

  if(mpg123_open(mp3->handle, file) != MPG123_OK){
    fprintf(stderr, "Failed to open %s: %s\n", file, mpg123_strerror(mp3->handle));
    goto cleanup;
  }
  
  if(mpg123_getformat(mp3->handle, &mp3_samplerate, &mp3_channels, &mp3_encoding) != MPG123_OK){
    fprintf(stderr, "Failed to get format properties of %s: %s\n", file, mpg123_strerror(mp3->handle));
    goto cleanup;
  }

  // test app limitation for now
  if(mp3_channels != 2){
    fprintf(stderr, "File %s has %i channels instead of 2. I can't deal with this.\n", file, mp3_channels);
    goto cleanup;
  }
  // libmixed limitation for now
  if(mp3_samplerate != 44100){
    fprintf(stderr, "File %s has a sample rate of %i Hz instead of 44100. I can't deal with this.\n", file, mp3_samplerate);
    goto cleanup;
  }
  
  mp3_encname = (char *)out123_enc_longname(mp3_encoding);
  fprintf(stderr, "MP3: %i channels @ %li Hz, %s\n", mp3_channels, mp3_samplerate, mp3_encname);

  mp3->pack.encoding = fmt123_to_mixed(mp3_encoding);
  mp3->pack.channels = mp3_channels;
  mp3->pack.layout = MIXED_ALTERNATING;
  mp3->pack.samplerate = mp3_samplerate;
  mp3_samplesize = mixed_samplesize(mp3->pack.encoding);
  mp3->pack.size = samples*mp3_samplesize*mp3_channels;
  mp3->pack.data = calloc(mp3->pack.size, sizeof(uint8_t));

  if(!mixed_make_segment_unpacker(&mp3->pack, 44100, &mp3->segment)){
    fprintf(stderr, "Failed to create segment for %s: %s\n", file, mixed_error_string(-1));
    goto cleanup;
  }

  if(!mixed_make_buffer(samples, &mp3->left) ||
     !mixed_make_buffer(samples, &mp3->right)){
    fprintf(stderr, "Failed to allocate mixer buffers: %s\n", mixed_error_string(-1));
    goto cleanup;
  }

  if(!mixed_segment_set_out(MIXED_BUFFER, MIXED_LEFT, &mp3->left, &mp3->segment) ||
     !mixed_segment_set_out(MIXED_BUFFER, MIXED_RIGHT, &mp3->right, &mp3->segment)){
    fprintf(stderr, "Failed to set buffers for %s: %s\n", file, mixed_error_string(-1));
    goto cleanup;
  }

  *_mp3 = mp3;
  return 1;

 cleanup:
  free_mp3(mp3);
  return 0;
}
