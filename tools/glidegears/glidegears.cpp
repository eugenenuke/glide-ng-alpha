#include <glide.h>
#include <math.h>
#include <sst1vid.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <tlib.h>

#include <chrono>
#include <iomanip>
#include <iostream>
#include <string>
#include <vector>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

// Fallback for Glide 3 which does not define depth constants in headers
#ifndef GR_WDEPTHVALUE_FARTHEST
#define GR_WDEPTHVALUE_FARTHEST 0xFFFF
#endif

#include <DiagnosticInfo.h>

// ======================================================================
// Lightweight CPU 3D Graphics Pipeline
// ======================================================================
struct Vector3D {
  float x, y, z;
};

struct Triangle {
  int v0, v1, v2;
};

struct ShadedTriangle {
  GrVertex v0, v1, v2;
  float avg_depth;
};

Vector3D RotateX(Vector3D v, float angle) {
  float s = sinf(angle);
  float c = cosf(angle);
  return {v.x, v.y * c - v.z * s, v.y * s + v.z * c};
}

Vector3D RotateY(Vector3D v, float angle) {
  float s = sinf(angle);
  float c = cosf(angle);
  return {v.x * c + v.z * s, v.y, -v.x * s + v.z * c};
}

Vector3D RotateZ(Vector3D v, float angle) {
  float s = sinf(angle);
  float c = cosf(angle);
  return {v.x * c - v.y * s, v.x * s + v.y * c, v.z};
}

Vector3D GetNormal(Vector3D p1, Vector3D p2, Vector3D p3) {
  Vector3D u = {p2.x - p1.x, p2.y - p1.y, p2.z - p1.z};
  Vector3D v = {p3.x - p1.x, p3.y - p1.y, p3.z - p1.z};
  Vector3D normal = {u.y * v.z - u.z * v.y, u.z * v.x - u.x * v.z,
                     u.x * v.y - u.y * v.x};
  float len =
      sqrtf(normal.x * normal.x + normal.y * normal.y + normal.z * normal.z);
  if (len > 0.0001f) {
    normal.x /= len;
    normal.y /= len;
    normal.z /= len;
  }
  return normal;
}

// ======================================================================
// 3D Gears Mesh Procedural Generation
// ======================================================================
struct GearMesh {
  std::vector<Vector3D> vertices;
  std::vector<Triangle> triangles;
  float r, g, b;
  float offset_x, offset_y;
  float speed_multiplier;
  float initial_angle_offset;
};

void GenerateGear(GearMesh& gear, float inner_radius, float outer_radius,
                  float width, int teeth, float tooth_depth) {
  float r0 = inner_radius;
  float r1 = outer_radius - tooth_depth / 2.0f;
  float r2 = outer_radius + tooth_depth / 2.0f;
  float da = 2.0f * (float)M_PI / teeth / 4.0f;

  // Front/Back faces
  for (int i = 0; i < teeth; i++) {
    float angle = i * 2.0f * (float)M_PI / teeth;
    for (int side = 0; side < 2; side++) {
      float z = (side == 0) ? -width / 2.0f : width / 2.0f;
      int base = gear.vertices.size();

      // Vertices for this tooth segment
      // Inner circle
      gear.vertices.push_back({r0 * cosf(angle), r0 * sinf(angle), z});
      gear.vertices.push_back(
          {r0 * cosf(angle + 3.0f * da), r0 * sinf(angle + 3.0f * da), z});
      // Outer circle / teeth geometry
      gear.vertices.push_back({r1 * cosf(angle), r1 * sinf(angle), z});
      gear.vertices.push_back(
          {r2 * cosf(angle + da), r2 * sinf(angle + da), z});
      gear.vertices.push_back(
          {r2 * cosf(angle + 2.0f * da), r2 * sinf(angle + 2.0f * da), z});
      gear.vertices.push_back(
          {r1 * cosf(angle + 3.0f * da), r1 * sinf(angle + 3.0f * da), z});

      // Triangles for front/back faces
      if (side == 0) {
        // Front face triangles (counter-clockwise orientation)
        gear.triangles.push_back({base, base + 2, base + 1});
        gear.triangles.push_back({base + 1, base + 2, base + 5});
        gear.triangles.push_back({base + 2, base + 3, base + 4});
        gear.triangles.push_back({base + 2, base + 4, base + 5});
      } else {
        // Back face triangles (clockwise orientation)
        gear.triangles.push_back({base, base + 1, base + 2});
        gear.triangles.push_back({base + 1, base + 5, base + 2});
        gear.triangles.push_back({base + 2, base + 4, base + 3});
        gear.triangles.push_back({base + 2, base + 5, base + 4});
      }
    }
  }

  // Outer teeth & cylinders (connecting front and back faces)
  for (int i = 0; i < teeth; i++) {
    int base_front = i * 12;
    int base_back = base_front + 6;

    // Draw quad panels connecting front and back edges
    // 3 outer panels per tooth segment
    int idx_f[4] = {2, 3, 4, 5};
    int idx_b[4] = {8, 9, 10, 11};

    for (int j = 0; j < 3; j++) {
      gear.triangles.push_back({base_front + idx_f[j],
                                base_front + idx_f[j + 1],
                                base_front + idx_b[j]});
      gear.triangles.push_back({base_front + idx_f[j + 1],
                                base_front + idx_b[j + 1],
                                base_front + idx_b[j]});
    }

    // Connection to next tooth
    int next_front = ((i + 1) % teeth) * 12;
    int next_f2 = next_front + 2;
    int next_b2 = next_front + 8;

    gear.triangles.push_back({base_front + 5, next_f2, base_front + 11});
    gear.triangles.push_back({next_f2, next_b2, base_front + 11});

    // Inner cylinder (axle hole)
    int next_back = next_front + 6;
    gear.triangles.push_back({base_front, base_back, base_front + 1});
    gear.triangles.push_back({base_front + 1, base_back, base_back + 1});
    gear.triangles.push_back({base_front + 1, base_back + 1, next_front});
    gear.triangles.push_back({next_front, base_back + 1, next_back});
  }
}

// ======================================================================
// Main Application & Window Loop
// ======================================================================
int main(int argc, char* argv[]) {
  // Initialize Glide, parse CLI, print header, and resolve resolution
  auto runConfig =
      Tools::InitializeAndParse(argc, argv, "Glide 3D Gears",
                                {"[ESC]          Exit Tool",
                                 "[ R ]          Toggle Continuous Rotation"});

  int max_frames = 0;
  float max_duration = 0.0f;
  for (int i = 1; i < argc; i++) {
    if ((strcmp(argv[i], "--frames") == 0 || strcmp(argv[i], "-frames") == 0 ||
         strcmp(argv[i], "-f") == 0) &&
        i + 1 < argc) {
      max_frames = std::atoi(argv[i + 1]);
      i++;
    } else if ((strcmp(argv[i], "--duration") == 0 ||
                strcmp(argv[i], "-duration") == 0 ||
                strcmp(argv[i], "-d") == 0) &&
               i + 1 < argc) {
      max_duration = std::atof(argv[i + 1]);
      i++;
    }
  }

  // Open Window & Initialize Graphics State
  grSstSelect(0);
#if GLIDE_VERSION == 3
  GrContext_t context =
      grSstWinOpen(0, runConfig.resEnum, GR_REFRESH_60Hz, GR_COLORFORMAT_ARGB,
                   GR_ORIGIN_LOWER_LEFT, 2, 1);
  if (!context) {
    std::cerr << "CRITICAL ERROR: Failed to open Glide 3 window context!"
              << std::endl;
    grGlideShutdown();
    return EXIT_FAILURE;
  }
#else
  FxBool win_opened =
      grSstWinOpen(0, runConfig.resEnum, GR_REFRESH_60Hz, GR_COLORFORMAT_ARGB,
                   GR_ORIGIN_LOWER_LEFT, 2, 1);
  if (!win_opened) {
    std::cerr << "CRITICAL ERROR: Failed to open Glide 2 window!" << std::endl;
    grGlideShutdown();
    return EXIT_FAILURE;
  }
#endif

  // Configure the scissor clip window to the full logical resolution size
  grClipWindow(0, 0, runConfig.width, runConfig.height);

  // Set standard Glide states for 3D flat-shaded, depth-buffered polygons
  grDepthBufferMode(GR_DEPTHBUFFER_WBUFFER);
  grDepthMask(FXTRUE);
  grDepthBufferFunction(GR_CMP_LESS);

  grColorCombine(GR_COMBINE_FUNCTION_LOCAL, GR_COMBINE_FACTOR_NONE,
                 GR_COMBINE_LOCAL_ITERATED, GR_COMBINE_OTHER_NONE, FXFALSE);
  grCullMode(GR_CULL_NEGATIVE);

  // Generate Gears Geometry
  std::vector<GearMesh> gears(3);
  // Red Gear
  gears[0].r = 0.8f;
  gears[0].g = 0.1f;
  gears[0].b = 0.1f;
  gears[0].offset_x = -1.2f;
  gears[0].offset_y = -0.2f;
  gears[0].speed_multiplier = 1.0f;
  gears[0].initial_angle_offset = 0.0f;
  GenerateGear(gears[0], 1.0f, 2.0f, 0.5f, 20, 0.7f);

  // Green Gear
  gears[1].r = 0.1f;
  gears[1].g = 0.8f;
  gears[1].b = 0.1f;
  gears[1].offset_x = 1.2f;
  gears[1].offset_y = -2.0f;
  gears[1].speed_multiplier = -2.0f;
  gears[1].initial_angle_offset = -9.0f * (float)M_PI / 180.0f;
  GenerateGear(gears[1], 0.5f, 1.0f, 0.5f, 10, 0.7f);

  // Blue Gear
  gears[2].r = 0.1f;
  gears[2].g = 0.1f;
  gears[2].b = 0.8f;
  gears[2].offset_x = 1.2f;
  gears[2].offset_y = 2.0f;
  gears[2].speed_multiplier = -2.0f;
  gears[2].initial_angle_offset = -25.0f * (float)M_PI / 180.0f;
  GenerateGear(gears[2], 0.5f, 1.0f, 0.5f, 10, 0.7f);

  // Frame-rate and timing loop states
  auto last_fps_report = std::chrono::steady_clock::now();
  auto start_time = std::chrono::steady_clock::now();
  int frame_count = 0;
  int total_frames_rendered = 0;
  float rotation_angle = 0.0f;

  float view_angle_x = 20.0f * (float)M_PI / 180.0f;
  float view_angle_y = 30.0f * (float)M_PI / 180.0f;

  bool spin = true;
  int screen_w = runConfig.width;
  int screen_h = runConfig.height;

  // Main Interactive Loop
  while (tlOkToRender()) {
    // Poll Input Events
    while (tlKbHit()) {
      char key = tlGetCH();
      if (key == 0x1b) {  // ESC key
        goto exit_loop;
      } else if (key == 'r' || key == 'R') {
        spin = !spin;
      }
    }

    // Update physics / rotation
    if (spin) {
      rotation_angle += 0.02f;
      if (rotation_angle > 2.0f * (float)M_PI) {
        rotation_angle -= 2.0f * (float)M_PI;
      }
    }

    // Clear Color Buffer (Black) and Depth Buffer
    grBufferClear(0x00000000, 0, GR_WDEPTHVALUE_FARTHEST);

    // Simple static lighting directional vector
    Vector3D light_dir = {0.577f, 0.577f, -0.577f};

    // Camera Depth settings
    float camera_dist = 15.0f;
    float proj_scale = 800.0f * (screen_h / 480.0f);

    for (const auto& gear : gears) {
      float gear_angle =
          rotation_angle * gear.speed_multiplier + gear.initial_angle_offset;

      for (const auto& tri : gear.triangles) {
        Vector3D raw_v0 = gear.vertices[tri.v0];
        Vector3D raw_v1 = gear.vertices[tri.v1];
        Vector3D raw_v2 = gear.vertices[tri.v2];

        // 1. Rotate around gear local axis (Z axis)
        raw_v0 = RotateZ(raw_v0, gear_angle);
        raw_v1 = RotateZ(raw_v1, gear_angle);
        raw_v2 = RotateZ(raw_v2, gear_angle);

        // Apply gear translation
        raw_v0.x += gear.offset_x;
        raw_v0.y += gear.offset_y;
        raw_v1.x += gear.offset_x;
        raw_v1.y += gear.offset_y;
        raw_v2.x += gear.offset_x;
        raw_v2.y += gear.offset_y;

        // 2. Rotate around global Camera/View axes (X then Y)
        Vector3D v0 = RotateY(RotateX(raw_v0, view_angle_x), view_angle_y);
        Vector3D v1 = RotateY(RotateX(raw_v1, view_angle_x), view_angle_y);
        Vector3D v2 = RotateY(RotateX(raw_v2, view_angle_x), view_angle_y);

        // Calculate normal of the transformed triangle for flat lighting
        Vector3D normal = GetNormal(v0, v1, v2);

        // Compute simple diffuse lighting factor
        float diffuse = normal.x * light_dir.x + normal.y * light_dir.y +
                        normal.z * light_dir.z;
        if (diffuse < 0.0f) diffuse = 0.0f;
        float intensity = 0.25f + 0.75f * diffuse;

        // Apply lighting to gear base colors
        float r = gear.r * intensity * 255.0f;
        float g = gear.g * intensity * 255.0f;
        float b = gear.b * intensity * 255.0f;

        // 3. Project to 2D screen space with perspective correct depth
        // (w-buffer)
        float d0 = v0.z + camera_dist;
        float d1 = v1.z + camera_dist;
        float d2 = v2.z + camera_dist;

        if (d0 <= 0.1f || d1 <= 0.1f || d2 <= 0.1f)
          continue;  // near-plane clipping

        GrVertex g_v0, g_v1, g_v2;

        g_v0.x = (screen_w / 2.0f) + (v0.x * proj_scale) / d0;
        g_v0.y = (screen_h / 2.0f) - (v0.y * proj_scale) / d0;
        g_v0.r = r;
        g_v0.g = g;
        g_v0.b = b;
        g_v0.a = 255.0f;
        g_v0.oow = 1.0f / d0;

        g_v1.x = (screen_w / 2.0f) + (v1.x * proj_scale) / d1;
        g_v1.y = (screen_h / 2.0f) - (v1.y * proj_scale) / d1;
        g_v1.r = r;
        g_v1.g = g;
        g_v1.b = b;
        g_v1.a = 255.0f;
        g_v1.oow = 1.0f / d1;

        g_v2.x = (screen_w / 2.0f) + (v2.x * proj_scale) / d2;
        g_v2.y = (screen_h / 2.0f) - (v2.y * proj_scale) / d2;
        g_v2.r = r;
        g_v2.g = g;
        g_v2.b = b;
        g_v2.a = 255.0f;
        g_v2.oow = 1.0f / d2;

        // Render via Glide entry point
        grDrawTriangle(&g_v0, &g_v1, &g_v2);
      }
    }

    // Swap Buffers to present to screen
    grBufferSwap(1);
    frame_count++;
    total_frames_rendered++;

    auto now = std::chrono::steady_clock::now();

    // Automated exit conditions
    if (max_frames > 0 && total_frames_rendered >= max_frames) {
      goto exit_loop;
    }
    if (max_duration > 0.0f) {
      std::chrono::duration<double> run_elapsed = now - start_time;
      if (run_elapsed.count() >= max_duration) {
        goto exit_loop;
      }
    }

    // Calculate and log FPS every 5.0 seconds
    std::chrono::duration<double> elapsed = now - last_fps_report;
    if (elapsed.count() >= 5.0) {
      double fps = frame_count / elapsed.count();
      printf("%d frames in %.3f seconds = %.3f FPS\r\n", frame_count,
             elapsed.count(), fps);
      fflush(stdout);

      frame_count = 0;
      last_fps_report = now;
    }
  }

exit_loop:
  // Clean Window Shutdown
#if GLIDE_VERSION == 3
  grSstWinClose(context);
#else
  grSstWinClose();
#endif

  grGlideShutdown();
  std::cout << "Glide Gears exited cleanly.\r\n";
  return EXIT_SUCCESS;
}
