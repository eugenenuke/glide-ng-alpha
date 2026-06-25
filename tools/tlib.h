#ifndef TOOLS_TLIB_H
#define TOOLS_TLIB_H

#ifdef __cplusplus
extern "C" {
#endif

int tlOkToRender(void);
int tlKbHit(void);
char tlGetCH(void);
void tlSetScreen(float width, float height);
float tlScaleX(float coord);
float tlScaleY(float coord);

#ifdef __cplusplus
}
#endif

// Define GrVertex and helper structures for Glide 3 compatibility
#if defined(GLIDE_VERSION) && GLIDE_VERSION == 3

#ifndef GLIDE_NUM_TMU
#define GLIDE_NUM_TMU 2
#endif

typedef struct {
  float  sow;                   /* s texture ordinate (s over w) */
  float  tow;                   /* t texture ordinate (t over w) */  
  float  oow;                   /* 1/w */
} GrTmuVertex;

typedef struct
{
  float x, y;         /* X and Y in screen space */
  float ooz;          /* 65535/Z (used for Z-buffering) */
  float oow;          /* 1/W (used for W-buffering, texturing) */
  float r, g, b, a;   /* R, G, B, A [0..255.0] */
  float z;            /* Z is ignored */
  GrTmuVertex  tmuvtx[GLIDE_NUM_TMU];
} GrVertex;

#endif

#endif // TOOLS_TLIB_H
