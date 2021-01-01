#include <err.h>
#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>

#include "bmp.h"
#include "camera.h"
#include "image.h"
#include "normal_material.h"
#include "obj_loader.h"
#include "phong_material.h"
#include "scene.h"
#include "sphere.h"
#include "triangle.h"
#include "vec3.h"
#include "color.h"


static void build_test_scene(struct scene *scene, double aspect_ratio)
{
    // create a sample red material
    struct phong_material *red_material = zalloc(sizeof(*red_material));
    phong_material_init(red_material);
    red_material->surface_color = light_from_rgb_color(191, 32, 32);
    red_material->diffuse_Kn = 0.2;
    red_material->spec_n = 10;
    red_material->spec_Ks = 0.2;
    red_material->ambient_intensity = 0.1;

    // create a single sphere with the above material, and add it to the scene
    struct sphere *sample_sphere
        = sphere_create((struct vec3){0, 10, 0}, 4, &red_material->base);
    object_vect_push(&scene->objects, &sample_sphere->base);

    // go the same with a triangle
    // points are listed counter-clockwise
    //     a
    //    /|
    //   / |
    //  b--c
    struct vec3 points[3] = {
        {6, 10, 1}, // a
        {5, 10, 0}, // b
        {6, 10, 0}, // c
    };

    struct triangle *sample_triangle
        = triangle_create(points, &red_material->base);
    object_vect_push(&scene->objects, &sample_triangle->base);

    // setup the scene lighting
    scene->light_intensity = 5;
    scene->light_color = light_from_rgb_color(255, 255, 0); // yellow
    scene->light_direction = (struct vec3){-1, 1, -1};
    vec3_normalize(&scene->light_direction);

    // setup the camera
    double cam_width = 10;
    double cam_height = cam_width / aspect_ratio;

    scene->camera = (struct camera){
        .center = {0, 0, 0},
        .forward = {0, 1, 0},
        .up = {0, 0, 1},
        .width = cam_width,
        .height = cam_height,
        .focal_distance = focal_distance_from_fov(cam_width, 80),
    };

    // release the reference to the material
    material_put(&red_material->base);
}

static void build_obj_scene(struct scene *scene, double aspect_ratio)
{
    // setup the scene lighting
    scene->light_intensity = 5;
    scene->light_color = light_from_rgb_color(255, 255, 0); // yellow
    scene->light_direction = (struct vec3){-1, -1, -1};
    vec3_normalize(&scene->light_direction);

    // setup the camera
    double cam_width = 2;
    double cam_height = cam_width / aspect_ratio;

    // for some reason the object points in the z axis,
    // with its up on y
    scene->camera = (struct camera){
        .center = {0, 1, 2},
        .forward = {0, -1, -2},
        .up = {0, 1, 0},
        .width = cam_width,
        .height = cam_height,
        .focal_distance = focal_distance_from_fov(cam_width, 40),
    };

    vec3_normalize(&scene->camera.forward);
    vec3_normalize(&scene->camera.up);
}

static struct ray image_cast_ray(const struct rgb_image *image,
                                 const struct scene *scene, size_t x, size_t y)
{
    // find the position of the current pixel in the image plane
    // camera_cast_ray takes camera relative positions, from -0.5 to 0.5 for
    // both axis
    double cam_x = ((double)x / image->width) - 0.5;
    double cam_y = ((double)y / image->height) - 0.5;

    // find the starting point and direction of this ray
    struct ray ray;
    camera_cast_ray(&ray, &scene->camera, cam_x, cam_y);
    return ray;
}

static double
scene_intersect_ray(struct object_intersection *closest_intersection,
                    struct scene *scene, struct ray *ray)
{
    // we will now try to find the closest object in the scene
    // intersecting this ray
    double closest_intersection_dist = INFINITY;

    for (size_t i = 0; i < object_vect_size(&scene->objects); i++)
    {
        struct object *obj = object_vect_get(&scene->objects, i);
        struct object_intersection intersection;
        // if there's no intersection between the ray and this object, skip it
        double intersection_dist = obj->intersect(&intersection, obj, ray);
        if (intersection_dist >= closest_intersection_dist)
            continue;

        closest_intersection_dist = intersection_dist;
        *closest_intersection = intersection;
    }

    return closest_intersection_dist;
}

typedef void (*render_mode_f)(struct rgb_image *, struct scene *, size_t x,
                              size_t y);

/* For all the pixels of the image, try to find the closest object
** intersecting the camera ray. If an object is found, shade the pixel to
** find its color.
*/
static void render_shaded(struct rgb_image *image, struct scene *scene,
                          size_t x, size_t y)
{
    struct ray ray = image_cast_ray(image, scene, x, y);

    struct object_intersection closest_intersection;
    double closest_intersection_dist
        = scene_intersect_ray(&closest_intersection, scene, &ray);

    // if the intersection distance is infinite, do not shade the pixel
    if (isinf(closest_intersection_dist))
        return;

    struct material *mat = closest_intersection.material;
    struct vec3 pix_color
        = mat->shade(mat, &closest_intersection.location, scene, &ray);
    rgb_image_set(image, x, y, rgb_color_from_light(&pix_color));
}

/* For all the pixels of the image, try to find the closest object
** intersecting the camera ray. If an object is found, shade the pixel to
** find its color.
*/
static void render_normals(struct rgb_image *image, struct scene *scene,
                           size_t x, size_t y)
{
    struct ray ray = image_cast_ray(image, scene, x, y);

    struct object_intersection closest_intersection;
    double closest_intersection_dist
        = scene_intersect_ray(&closest_intersection, scene, &ray);

    // if the intersection distance is infinite, do not shade the pixel
    if (isinf(closest_intersection_dist))
        return;

    struct material *mat = closest_intersection.material;
    struct vec3 pix_color = normal_material.shade(
        mat, &closest_intersection.location, scene, &ray);
    rgb_image_set(image, x, y, rgb_color_from_light(&pix_color));
}

/* For all the pixels of the image, try to find the closest object
** intersecting the camera ray. If an object is found, shade the pixel to
** find its color.
*/
static void render_distances(struct rgb_image *image, struct scene *scene,
                             size_t x, size_t y)
{
    struct ray ray = image_cast_ray(image, scene, x, y);

    struct object_intersection closest_intersection;
    double closest_intersection_dist
        = scene_intersect_ray(&closest_intersection, scene, &ray);

    // if the intersection distance is infinite, do not shade the pixel
    if (isinf(closest_intersection_dist))
        return;

    assert(closest_intersection_dist > 0);

    // distance from 0 to +inf
    // we want something from 0 to 1
    double depth_repr = 1 / (closest_intersection_dist + 1);
    uint8_t depth_intensity = depth_repr * 255;
    struct rgb_pixel pix_color
        = {depth_intensity, depth_intensity, depth_intensity};
    rgb_image_set(image, x, y, pix_color);
}

struct thread_info
{
    size_t thread_num;
    pthread_t thread_id;

    size_t y_s;
    size_t y_e;

    struct scene *scene;
    struct rgb_image *image;
    render_mode_f renderer;
};

static void *thread_render(void *arg)
{
    struct thread_info *tinfo = arg;
    // printf("Thread %zu takes charge with line %zu to %zu\n", tinfo->thread_num, tinfo->y_s, tinfo->y_e - 1);
    size_t s = tinfo->y_s;
    size_t e = tinfo->y_e;

    struct scene *scene = tinfo->scene;
    struct rgb_image *image = tinfo->image;

    for (size_t y = s; y < e; y++)
        for (size_t x = 0; x < image->width; x++)
            tinfo->renderer(image, scene, x, y);

    return NULL;
}

int main(int argc, char *argv[])
{
    int rc;

    if (argc < 3)
        errx(1, "Usage: SCENE.obj OUTPUT.bmp [--normals] [--distances]");

    struct scene scene;
    scene_init(&scene);

    // initialize the frame buffer (the buffer that will store the result of the
    // rendering)
    struct rgb_image *image = rgb_image_alloc(1000, 1000);

    // set all the pixels of the image to black
    struct rgb_pixel bg_color = {0};
    rgb_image_clear(image, &bg_color);

    double aspect_ratio = (double)image->width / image->height;

    // build the scene
    build_obj_scene(&scene, aspect_ratio);

    if (load_obj(&scene, argv[1]))
        return 41;

    // parse options
    render_mode_f renderer = render_shaded;
    for (int i = 3; i < argc; i++)
    {
        if (strcmp(argv[i], "--normals") == 0)
            renderer = render_normals;
        else if (strcmp(argv[i], "--distances") == 0)
            renderer = render_distances;
    }

    // // render all pixels
    // for (size_t y = 0; y < image->height; y++)
    //     for (size_t x = 0; x < image->width; x++)
    //         renderer(image, &scene, x, y);

    // multithreading
    size_t num_threads = sysconf(_SC_NPROCESSORS_ONLN);
    // puts("Number of processors available is:");
    // printf("- %zu\n", num_threads);
    // size_t num_threads = 2;

    struct thread_info *tinfo = xcalloc(num_threads, sizeof(struct thread_info));

    int res;
    for (size_t tnum = 0; tnum < num_threads; tnum++)
    {
        tinfo[tnum].thread_num = tnum + 1;

        tinfo[tnum].y_s = tnum * image->height / num_threads;
        tinfo[tnum].y_e = (tnum + 1) * image->height / num_threads;

        tinfo[tnum].scene = &scene;
        tinfo[tnum].image = image;
        tinfo[tnum].renderer = renderer;

        res = pthread_create(&tinfo[tnum].thread_id, NULL, &thread_render, &tinfo[tnum]);
        if (res != 0)
            errx(42, "pthread_create error, exiting...");
    }

    void *retval;
    for (size_t tnum = 0; tnum < num_threads; tnum++)
    {
        res = pthread_join(tinfo[tnum].thread_id, &retval);
        if (res != 0)
            errx(42, "pthread_join error, exiting...");

        // printf("Joined with thread %zu, return value was %s\n", tinfo[tnum].thread_num, (char *)retval);
        free(retval);
    }
    free(tinfo);

    // write the rendered image to a bmp file
    FILE *fp = fopen(argv[2], "w");
    if (fp == NULL)
        err(1, "failed to open the output file");

    rc = bmp_write(image, ppm_from_ppi(80), fp);
    fclose(fp);

    // release resources
    scene_destroy(&scene);
    free(image);
    return rc;
}
