#pragma once

typedef struct rawimage_t
{
  char make[32];
  char model[32];
  char clean_make[32];
  char clean_model[32];
  uint64_t width;
  uint64_t height;
  uint64_t cpp;
  float wb_coeffs[4];
  uint16_t whitelevels[4];
  uint16_t blacklevels[4];
  float xyz_to_cam[4][3];
  uint32_t filters;
  uint64_t crop_aabb[4];
  uint32_t orientation;
  void *data;
} rawimage_t;

uint64_t rl_decode_file(
    const char *filename,
    rawimage_t *rawimg);
