// license:BSD-3-Clause
// copyright-holders:Olivier Galibert
// Standalone software display-list renderer for Sega Model 1.
#pragma once

#include <glm/geometric.hpp>
#include <glm/vec3.hpp>

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <functional>
#include <memory>
#include <vector>

struct rectangle {
    int min_x, max_x, min_y, max_y;
};

class bitmap_rgb32 {
public:
    bitmap_rgb32(int width, int height)
        : m_width(width), m_height(height),
          m_pixels(static_cast<std::size_t>(width * height), 0) {}
    uint32_t& pix32(int y, int x = 0) {
        return m_pixels[static_cast<std::size_t>(y * m_width + x)];
    }
    const std::vector<uint32_t>& pixels() const { return m_pixels; }
    void fill(uint32_t color, const rectangle& clip) {
        for (int y = clip.min_y; y <= clip.max_y; ++y)
            std::fill_n(&pix32(y, clip.min_x),
                        clip.max_x - clip.min_x + 1, color);
    }

private:
    int m_width;
    int m_height;
    std::vector<uint32_t> m_pixels;
};

inline float model1_video_u2f(uint32_t value) {
    union { uint32_t integer; float real; } bits{value};
    return bits.real;
}
inline uint8_t pal5bit(unsigned value) {
    return static_cast<uint8_t>((value << 3) | (value >> 2));
}

class model1_video {
public:
    static constexpr int width = 496;
    static constexpr int height = 384;

    model1_video();
    void set_polygon_rom(const std::vector<uint8_t>& bytes);
    void render(const std::vector<uint8_t>& display_list_0,
                const std::vector<uint8_t>& display_list_1,
                const std::vector<uint8_t>& tile_ram,
                const std::vector<uint8_t>& character_ram,
                const std::vector<uint8_t>& palette,
                const std::vector<uint8_t>& color_translation,
                uint16_t list_control_0, uint16_t list_control_1);
    const std::vector<uint8_t>& rgba_pixels() const { return m_rgba_pixels; }
    std::size_t last_quad_count() const { return m_last_quad_count; }
    uint32_t last_unknown_list_type() const { return m_last_unknown_list_type; }
    uint16_t effective_list_control_0() const { return m_listctl[0]; }

    struct spoint_t { int32_t x, y; };
    struct point_t {
        float x, y, z;
        float xx, yy;
        spoint_t s;
    };
    class quad_t {
    public:
        quad_t() = default;
        quad_t(int ccol, float cz, point_t* p0, point_t* p1,
               point_t* p2, point_t* p3)
            : p{p0, p1, p2, p3}, z(cz), col(ccol) {}
        int compare(const quad_t* other) const;
        point_t* p[4]{nullptr, nullptr, nullptr, nullptr};
        float z{0};
        int col{0};
    };
    struct lightparam_t {
        float a{0}, d{0}, s{0};
        int p{0};
    };
    class view_t {
    public:
        view_t() : light(0, 0, 0) {}
        void init_translation_matrix();
        void set_viewport(float, float, float, float, float, float);
        void set_lightparam(int, float, float, float, int);
        void set_zoom(float, float);
        void set_light_direction(float, float, float);
        void set_translation_matrix(float*);
        void set_view_translation(float, float);
        void project_point(point_t*) const;
        void project_point_direct(point_t*) const;
        void transform_vector(glm::vec3&) const;
        void transform_point(point_t*) const;
        void recompute_frustum();
        int xc{0}, yc{0}, x1{0}, y1{0}, x2{0}, y2{0};
        float zoomx{0}, zoomy{0}, viewx{0}, viewy{0};
        float a_bottom{0}, a_top{0}, a_left{0}, a_right{0};
        float vxx{0}, vyy{0}, vzz{0}, ayy{0}, ayyc{0}, ayys{0};
        float translation[12]{};
        glm::vec3 light;
        lightparam_t lightparams[32];
    };

private:
    class clipper_t {
    public:
        std::function<bool(view_t*, point_t*)> m_isclipped;
        std::function<void(view_t*, point_t*, point_t*, point_t*)> m_clip;
    };

    uint32_t readi(int) const;
    int16_t readi16(int) const;
    float readf(int) const;
    void cross_product(point_t*, const point_t*, const point_t*) const;
    float view_determinant(const point_t*, const point_t*, const point_t*) const;
    static void draw_hline(bitmap_rgb32&, int, int, int, int);
    static void draw_hline_moired(bitmap_rgb32&, int, int, int, int);
    static void fill_slope(bitmap_rgb32&, view_t*, int, int32_t, int32_t,
                           int32_t, int32_t, int32_t, int32_t, int32_t*, int32_t*);
    static void fill_line(bitmap_rgb32&, view_t*, int, int32_t, int32_t, int32_t);
    void fill_quad(bitmap_rgb32&, view_t*, const quad_t&) const;
    void sort_quads() const;
    void unsort_quads() const;
    void draw_quads(bitmap_rgb32&, const rectangle&);
    static bool fclip_isc_near(view_t*, point_t*);
    static bool fclip_isc_bottom(view_t*, point_t*);
    static bool fclip_isc_top(view_t*, point_t*);
    static bool fclip_isc_left(view_t*, point_t*);
    static bool fclip_isc_right(view_t*, point_t*);
    static void fclip_clip_near(view_t*, point_t*, point_t*, point_t*);
    static void fclip_clip_bottom(view_t*, point_t*, point_t*, point_t*);
    static void fclip_clip_top(view_t*, point_t*, point_t*, point_t*);
    static void fclip_clip_left(view_t*, point_t*, point_t*, point_t*);
    static void fclip_clip_right(view_t*, point_t*, point_t*, point_t*);
    void fclip_push_quad_next(int, quad_t&, point_t*, point_t*, point_t*, point_t*);
    void fclip_push_quad(int, quad_t&);
    static float min4f(float, float, float, float);
    static float max4f(float, float, float, float);
    static float compute_specular(glm::vec3&, glm::vec3&, float, int);
    int push_direct(int);
    int draw_direct(bitmap_rgb32&, const rectangle&, int);
    int skip_direct(int) const;
    void push_object(uint32_t, uint32_t, uint32_t, float& old_z);
    void draw_objects(bitmap_rgb32&, const rectangle&);
    void set_current_render_list();
    int get_list_number();
    void end_frame();
    void tgp_render(bitmap_rgb32&, const rectangle&);
    void tgp_scan();
    void video_start();
    void draw_tile_pair(bitmap_rgb32& bitmap,
                        const std::vector<uint8_t>& tile_ram,
                        const std::vector<uint8_t>& character_ram,
                        int pair, int category, bool opaque) const;

    std::unique_ptr<view_t> m_view;
    std::vector<point_t> m_point_storage;
    std::vector<quad_t> m_quad_storage;
    mutable std::vector<quad_t*> m_quad_index_storage;
    point_t* m_pointdb{nullptr};
    point_t* m_pointpt{nullptr};
    quad_t* m_quaddb{nullptr};
    quad_t* m_quadpt{nullptr};
    quad_t** m_quadind{nullptr};
    std::vector<uint16_t> m_display_list0;
    std::vector<uint16_t> m_display_list1;
    uint16_t* m_display_list_current{nullptr};
    std::vector<uint32_t> m_poly_rom;
    std::vector<uint32_t> m_poly_ram;
    std::vector<uint16_t> m_tgp_ram;
    std::vector<uint16_t> m_paletteram16;
    std::vector<uint16_t> m_color_xlat;
    uint16_t m_listctl[2]{};
    bool m_render_done{false};
    uint64_t m_frame_number{0};
    clipper_t m_clipfn[5];
    bitmap_rgb32 m_bitmap{width, height};
    std::vector<uint8_t> m_rgba_pixels;
    std::size_t m_last_quad_count{0};
    uint32_t m_last_unknown_list_type{0};
};
