#include <err.h>
#include <math.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "bmp.h"
#include "camera.h"
#include "color.h"
#include "image.h"
#include "normal_material.h"
#include "obj_loader.h"
#include "phong_material.h"
#include "scene.h"
#include "sphere.h"
#include "triangle.h"
#include "vec3.h"

#define NUM_SAMPLES 4
#define MAX_DEPTH 10

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
    struct sphere *sample_sphere1
        = sphere_create((struct vec3){0, 10, 0}, 4, &red_material->base);
    object_vect_push(&scene->objects, &sample_sphere1->base);

    struct sphere *sample_sphere2
        = sphere_create((struct vec3){-7, 10, 0}, 3, &red_material->base);
    object_vect_push(&scene->objects, &sample_sphere2->base);

    struct sphere *sample_sphere3
        = sphere_create((struct vec3){0, 7, 6}, 3, &red_material->base);
    object_vect_push(&scene->objects, &sample_sphere3->base);

    // // go the same with a triangle
    // // points are listed counter-clockwise
    // //     a
    // //    /|
    // //   / |
    // //  b--c
    // struct vec3 points[3] = {
    //     {6, 10, 1}, // a
    //     {5, 10, 0}, // b
    //     {6, 10, 0}, // c
    // };

    // struct triangle *sample_triangle
    //     = triangle_create(points, &red_material->base);
    // object_vect_push(&scene->objects, &sample_triangle->base);

    // setup the scene lighting
    scene->light_intensity = 5;
    scene->light_color = light_from_rgb_color(255, 255, 255); // yellow
    scene->light_direction = (struct vec3){0, 1, -2};
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
    scene->light_color = light_from_rgb_color(255, 255, 255); // yellow
    scene->light_direction = (struct vec3){-1, -1, -1};
    vec3_normalize(&scene->light_direction);

    // setup the camera
    double cam_width = 7;
    double cam_height = cam_width / aspect_ratio;

    // for some reason the object points in the z axis,
    // with its up on y
    scene->camera = (struct camera){
        .center = {-0.5, 2, 2},
        .forward = {0.5, -1, -2},
        .up = {0, 1, 0},
        .width = cam_width,
        .height = cam_height,
        .focal_distance = focal_distance_from_fov(cam_width, 40),
    };

    vec3_normalize(&scene->camera.forward);
    vec3_normalize(&scene->camera.up);
}

static double random_double(double min, double max)
{
    // random double in [0, 1)
    // return rand() / (RAND_MAX + 1.0);

    // random double in [min, max)
    return min + (max - min) * rand() / (RAND_MAX + 1.0);
}

/**
** Cast certain number of sample rays for antialiasing
*/
static struct ray *image_cast_ray(const struct rgb_image *image,
                                  const struct scene *scene, size_t x, size_t y)
{
    struct ray *ray = xcalloc(NUM_SAMPLES, sizeof(struct ray));

    double cam_x;
    double cam_y;
    double u;
    double v;

    double rank = sqrt(NUM_SAMPLES);

    for (size_t i = 0; i < NUM_SAMPLES; i++)
    {
        /*
        ** +---+---+
        ** | * | * |
        ** +---+---+
        ** | * | * |
        ** +---+---+
        ** ex. 4 sample points deployed in the pixel grilled in 4
        ** same for 16, which is 4 * 4 in the 4 grill
        */
        if (i < NUM_SAMPLES / 2)
            v = y + random_double(0, 0.5);
        else
            v = y + random_double(0.5, 1);

        size_t l = i - i % (size_t)rank;
        if (i >= l && i < l + rank / 2)
            u = x + random_double(0, 0.5);
        else
            u = x + random_double(0.5, 1);

        cam_x = (u / image->width) - 0.5;
        cam_y = (v / image->height) - 0.5;

        camera_cast_ray(&ray[i], &scene->camera, cam_x, cam_y);
    }

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

typedef struct vec3 (*render_mode_f)(struct scene *, struct ray *ray, int depth);

static struct vec3 render_shaded(struct scene *scene, struct ray *ray, int depth)
{
    if (depth <= 0)
        return (struct vec3){0, 0, 0};

    struct object_intersection closest_intersection;
    double closest_intersection_dist
        = scene_intersect_ray(&closest_intersection, scene, ray);
    // if the intersection distance is infinite, do not shade the pixel
    if (isinf(closest_intersection_dist))
        return (struct vec3){0, 0, 0};

    struct ray reflect_ray;
    reflect_ray.direction = vec3_reflect(&(ray->direction), &(closest_intersection.location.normal));
    reflect_ray.source = closest_intersection.location.point;

    struct material *mat = closest_intersection.material;

    struct vec3 reflect_color = render_shaded(scene, &reflect_ray, depth - 1);
    reflect_color = vec3_mul(&reflect_color, 0.2);

    struct vec3 ori_color = mat->shade(mat, &closest_intersection.location, scene, ray);

    return vec3_add(&reflect_color, &ori_color);
}

/* For all the pixels of the image, try to find the closest object
** intersecting the camera ray. If an object is found, shade the pixel to
** find its color.
*/
static struct vec3 render_normals(struct scene *scene, struct ray *ray, int depth)
{
    depth = depth;
    struct object_intersection closest_intersection;
    double closest_intersection_dist
        = scene_intersect_ray(&closest_intersection, scene, ray);

    // if the intersection distance is infinite, do not shade the pixel
    if (isinf(closest_intersection_dist))
        return (struct vec3){0, 0, 0};

    struct material *mat = closest_intersection.material;
    struct vec3 pix_color = normal_material.shade(
        mat, &closest_intersection.location, scene, ray);

    return pix_color;
}

/* For all the pixels of the image, try to find the closest object
** intersecting the camera ray. If an object is found, shade the pixel to
** find its color.
*/
static struct vec3 render_distances(struct scene *scene, struct ray *ray, int depth)
{
    depth = depth;
    struct object_intersection closest_intersection;
    double closest_intersection_dist
        = scene_intersect_ray(&closest_intersection, scene, ray);

    // if the intersection distance is infinite, do not shade the pixel
    if (isinf(closest_intersection_dist))
        return (struct vec3){0, 0, 0};

    assert(closest_intersection_dist > 0);

    // distance from 0 to +inf
    // we want something from 0 to 1
    double depth_repr = 1 / (closest_intersection_dist + 1);
    struct vec3 pix_color = {depth_repr, depth_repr, depth_repr};

    return pix_color;
}

static void aa_render(render_mode_f renderer, struct rgb_image *image,
                      struct scene *scene, size_t x, size_t y)
{
    struct ray *ray = image_cast_ray(image, scene, x, y);
    struct vec3 pix_color = {0};
    struct vec3 sample_pix_color;
    for (size_t i = 0; i < NUM_SAMPLES; i++)
    {
        sample_pix_color = renderer(scene, &ray[i], MAX_DEPTH);
        pix_color = vec3_add(&pix_color, &sample_pix_color);
    }
    free(ray);

    double scale = 1.0 / NUM_SAMPLES;
    pix_color = vec3_mul(&pix_color, scale);

    rgb_image_set(image, x, y, rgb_color_from_light(&pix_color));
}

// Used as argument to thread_start()
struct thread_info
{
    size_t thread_num;
    pthread_t thread_id;

    size_t y_s; // Starting line of the image
    size_t y_e; // Ending line of the image

    struct scene *scene;
    struct rgb_image *image;
    render_mode_f renderer;
};

/**
** The render function of the starting thread,
** for each thread, it render only the lines specified by arg
*/
static void *thread_start(void *arg)
{
    struct thread_info *tinfo = arg;
    // printf("Thread %zu takes charge with line %zu to %zu\n",
    // tinfo->thread_num, tinfo->y_s, tinfo->y_e - 1);
    size_t s = tinfo->y_s;
    size_t e = tinfo->y_e;

    struct scene *scene = tinfo->scene;
    struct rgb_image *image = tinfo->image;

    for (size_t y = s; y < e; y++)
        for (size_t x = 0; x < image->width; x++)
            // tinfo->renderer(image, scene, x, y);
            aa_render(tinfo->renderer, image, scene, x, y);

    return NULL;
}

static void multithreading(struct rgb_image *image, struct scene *scene,
                           render_mode_f renderer)
{
    // multithreading depending on the number of available processors
    size_t num_threads = sysconf(_SC_NPROCESSORS_ONLN);
    // size_t num_threads = 2;

    // Allocate memory for the arguments of thread_start
    struct thread_info *tinfo
        = xcalloc(num_threads, sizeof(struct thread_info));

    // Create the corresponding threads
    int res;
    for (size_t tnum = 0; tnum < num_threads; tnum++)
    {
        tinfo[tnum].thread_num = tnum + 1;

        tinfo[tnum].y_s = tnum * image->height / num_threads;
        tinfo[tnum].y_e = (tnum + 1) * image->height / num_threads;

        tinfo[tnum].scene = scene;
        tinfo[tnum].image = image;
        tinfo[tnum].renderer = renderer;

        res = pthread_create(&tinfo[tnum].thread_id, NULL, &thread_start,
                             &tinfo[tnum]);
        if (res != 0)
            errx(42, "pthread_create error, exiting...");
    }
    // Wait all the threads joining back to the main
    void *retval;
    for (size_t tnum = 0; tnum < num_threads; tnum++)
    {
        res = pthread_join(tinfo[tnum].thread_id, &retval);
        if (res != 0)
            errx(42, "pthread_join error, exiting...");

        // printf("Joined with thread %zu, return value was %s\n",
        // tinfo[tnum].thread_num, (char *)retval);
        free(retval);
    }
    free(tinfo);
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

    // build_test_scene(&scene, aspect_ratio);

    // parse options
    render_mode_f renderer = render_shaded;
    for (int i = 3; i < argc; i++)
    {
        if (strcmp(argv[i], "--normals") == 0)
            renderer = render_normals;
        else if (strcmp(argv[i], "--distances") == 0)
            renderer = render_distances;
    }

    // render all pixels using multithreading
    multithreading(image, &scene, renderer);

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
