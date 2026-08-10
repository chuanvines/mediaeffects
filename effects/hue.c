/* hue.c - angle 180 (HALD LUT rotation in YUV color space) */
const char *const mediaeffects_hue_vf = "movie=<ccs_180.ppm>,[in]haldclut";
const char *const mediaeffects_hue_lut = "hald:6 -colorspace yuv -fx \"angle=180*pi/180;channel(u,.5+(u.g-.5)*cos(angle)-(u.b-.5)*sin(angle),.5+(u.g-.5)*sin(angle)+(u.b-.5)*cos(angle))\" -colorspace srgb";
