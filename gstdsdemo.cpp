/**
 * Copyright (c) 2017-2021, NVIDIA CORPORATION.  All rights reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */

#include <string.h>
#include <string>
#include <sstream>
#include <iostream>
#include <ostream>
#include <fstream>
#include "gstdsdemo.h"
#include <sys/time.h>

GST_DEBUG_CATEGORY_STATIC (gst_dsdemo_debug);
#define GST_CAT_DEFAULT gst_dsdemo_debug
#define USE_EGLIMAGE 1
/* enable to write transformed cvmat to files */
/* #define dsdemo_DEBUG */
static GQuark _dsmeta_quark = 0;

/* Enum to identify properties */
enum
{
  PROP_0,
  PROP_UNIQUE_ID,
  PROP_PROCESSING_WIDTH,
  PROP_PROCESSING_HEIGHT,
  PROP_PROCESS_FULL_FRAME,
  PROP_BLUR_OBJECTS,
  PROP_GPU_DEVICE_ID
};

#define CHECK_NVDS_MEMORY_AND_GPUID(object, surface)  \
  ({ int _errtype=0;\
   do {  \
    if ((surface->memType == NVBUF_MEM_DEFAULT || surface->memType == NVBUF_MEM_CUDA_DEVICE) && \
        (surface->gpuId != object->gpu_id))  { \
    GST_ELEMENT_ERROR (object, RESOURCE, FAILED, \
        ("Input surface gpu-id doesnt match with configured gpu-id for element," \
         " please allocate input using unified memory, or use same gpu-ids"),\
        ("surface-gpu-id=%d,%s-gpu-id=%d",surface->gpuId,GST_ELEMENT_NAME(object),\
         object->gpu_id)); \
    _errtype = 1;\
    } \
    } while(0); \
    _errtype; \
  })


/* Default values for properties */
#define DEFAULT_UNIQUE_ID 15
#define DEFAULT_PROCESSING_WIDTH 640
#define DEFAULT_PROCESSING_HEIGHT 480
#define DEFAULT_PROCESS_FULL_FRAME TRUE
#define DEFAULT_BLUR_OBJECTS FALSE
#define DEFAULT_GPU_ID 0

#define RGB_BYTES_PER_PIXEL 3
#define RGBA_BYTES_PER_PIXEL 4
#define Y_BYTES_PER_PIXEL 1
#define UV_BYTES_PER_PIXEL 2

#define MIN_INPUT_OBJECT_WIDTH 16
#define MIN_INPUT_OBJECT_HEIGHT 16

#define CHECK_NPP_STATUS(npp_status,error_str) do { \
  if ((npp_status) != NPP_SUCCESS) { \
    g_print ("Error: %s in %s at line %d: NPP Error %d\n", \
        error_str, __FILE__, __LINE__, npp_status); \
    goto error; \
  } \
} while (0)

#define CHECK_CUDA_STATUS(cuda_status,error_str) do { \
  if ((cuda_status) != cudaSuccess) { \
    g_print ("Error: %s in %s at line %d (%s)\n", \
        error_str, __FILE__, __LINE__, cudaGetErrorName(cuda_status)); \
    goto error; \
  } \
} while (0)

/* By default NVIDIA Hardware allocated memory flows through the pipeline. We
 * will be processing on this type of memory only. */
#define GST_CAPS_FEATURE_MEMORY_NVMM "memory:NVMM"
static GstStaticPadTemplate gst_dsdemo_sink_template =
GST_STATIC_PAD_TEMPLATE ("sink",
    GST_PAD_SINK,
    GST_PAD_ALWAYS,
    GST_STATIC_CAPS (GST_VIDEO_CAPS_MAKE_WITH_FEATURES
        (GST_CAPS_FEATURE_MEMORY_NVMM,
            "{ NV12, RGBA, I420 }")));

static GstStaticPadTemplate gst_dsdemo_src_template =
GST_STATIC_PAD_TEMPLATE ("src",
    GST_PAD_SRC,
    GST_PAD_ALWAYS,
    GST_STATIC_CAPS (GST_VIDEO_CAPS_MAKE_WITH_FEATURES
        (GST_CAPS_FEATURE_MEMORY_NVMM,
            "{ NV12, RGBA, I420 }")));

/* Define our element type. Standard GObject/GStreamer boilerplate stuff */
#define gst_dsdemo_parent_class parent_class
G_DEFINE_TYPE (GstDsDemo, gst_dsdemo, GST_TYPE_BASE_TRANSFORM);

static void gst_dsdemo_set_property (GObject * object, guint prop_id,
    const GValue * value, GParamSpec * pspec);
static void gst_dsdemo_get_property (GObject * object, guint prop_id,
    GValue * value, GParamSpec * pspec);

static gboolean gst_dsdemo_set_caps (GstBaseTransform * btrans,
    GstCaps * incaps, GstCaps * outcaps);
static gboolean gst_dsdemo_start (GstBaseTransform * btrans);
static gboolean gst_dsdemo_stop (GstBaseTransform * btrans);

static GstFlowReturn gst_dsdemo_transform_ip (GstBaseTransform *
    btrans, GstBuffer * inbuf);

/* Install properties, set sink and src pad capabilities, override the required
 * functions of the base class, These are common to all instances of the
 * element.
 */
static void
gst_dsdemo_class_init (GstDsDemoClass * klass)
{
  GObjectClass *gobject_class;
  GstElementClass *gstelement_class;
  GstBaseTransformClass *gstbasetransform_class;

  /* Indicates we want to use DS buf api */
  g_setenv ("DS_NEW_BUFAPI", "1", TRUE);

  gobject_class = (GObjectClass *) klass;
  gstelement_class = (GstElementClass *) klass;
  gstbasetransform_class = (GstBaseTransformClass *) klass;

  /* Overide base class functions */
  gobject_class->set_property = GST_DEBUG_FUNCPTR (gst_dsdemo_set_property);
  gobject_class->get_property = GST_DEBUG_FUNCPTR (gst_dsdemo_get_property);

  gstbasetransform_class->set_caps = GST_DEBUG_FUNCPTR (gst_dsdemo_set_caps);
  gstbasetransform_class->start = GST_DEBUG_FUNCPTR (gst_dsdemo_start);
  gstbasetransform_class->stop = GST_DEBUG_FUNCPTR (gst_dsdemo_stop);

  gstbasetransform_class->transform_ip =
      GST_DEBUG_FUNCPTR (gst_dsdemo_transform_ip);

  /* Install properties */
  g_object_class_install_property (gobject_class, PROP_UNIQUE_ID,
      g_param_spec_uint ("unique-id",
          "Unique ID",
          "Unique ID for the element. Can be used to identify output of the"
          " element", 0, G_MAXUINT, DEFAULT_UNIQUE_ID, (GParamFlags)
          (G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS)));

  g_object_class_install_property (gobject_class, PROP_PROCESSING_WIDTH,
      g_param_spec_int ("processing-width",
          "Processing Width",
          "Width of the input buffer to algorithm",
          1, G_MAXINT, DEFAULT_PROCESSING_WIDTH, (GParamFlags)
          (G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS)));

  g_object_class_install_property (gobject_class, PROP_PROCESSING_HEIGHT,
      g_param_spec_int ("processing-height",
          "Processing Height",
          "Height of the input buffer to algorithm",
          1, G_MAXINT, DEFAULT_PROCESSING_HEIGHT, (GParamFlags)
          (G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS)));

  g_object_class_install_property (gobject_class, PROP_PROCESS_FULL_FRAME,
      g_param_spec_boolean ("full-frame",
          "Full frame",
          "Enable to process full frame or disable to process objects detected"
          "by primary detector", DEFAULT_PROCESS_FULL_FRAME, (GParamFlags)
          (G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS)));

  g_object_class_install_property (gobject_class, PROP_BLUR_OBJECTS,
      g_param_spec_boolean ("blur-objects",
          "Blur Objects",
          "Enable to blur the objects detected in full-frame=0 mode"
          "by primary detector", DEFAULT_BLUR_OBJECTS, (GParamFlags)
          (G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS)));

  g_object_class_install_property (gobject_class, PROP_GPU_DEVICE_ID,
      g_param_spec_uint ("gpu-id",
          "Set GPU Device ID",
          "Set GPU Device ID", 0,
          G_MAXUINT, 0,
          GParamFlags
          (G_PARAM_READWRITE |
              G_PARAM_STATIC_STRINGS | GST_PARAM_MUTABLE_READY)));
  /* Set sink and src pad capabilities */
  gst_element_class_add_pad_template (gstelement_class,
      gst_static_pad_template_get (&gst_dsdemo_src_template));
  gst_element_class_add_pad_template (gstelement_class,
      gst_static_pad_template_get (&gst_dsdemo_sink_template));

  /* Set metadata describing the element */
  gst_element_class_set_details_simple (gstelement_class,
      "dsdemo plugin",
      "dsdemo Plugin",
      "Process a 3rdparty example algorithm on objects / full frame",
      "NVIDIA Corporation. Post on Deepstream for Tesla forum for any queries "
      "@ https://devtalk.nvidia.com/default/board/209/");
}

static void
gst_dsdemo_init (GstDsDemo * dsdemo)
{
  GstBaseTransform *btrans = GST_BASE_TRANSFORM (dsdemo);

  /* We will not be generating a new buffer. Just adding / updating
   * metadata. */
  gst_base_transform_set_in_place (GST_BASE_TRANSFORM (btrans), TRUE);
  /* We do not want to change the input caps. Set to passthrough. transform_ip
   * is still called. */
  gst_base_transform_set_passthrough (GST_BASE_TRANSFORM (btrans), TRUE);

  /* Initialize all property variables to default values */
  dsdemo->unique_id = DEFAULT_UNIQUE_ID;
  dsdemo->processing_width = DEFAULT_PROCESSING_WIDTH;
  dsdemo->processing_height = DEFAULT_PROCESSING_HEIGHT;
  dsdemo->process_full_frame = DEFAULT_PROCESS_FULL_FRAME;
  dsdemo->blur_objects = DEFAULT_BLUR_OBJECTS;
  dsdemo->gpu_id = DEFAULT_GPU_ID;

  /* This quark is required to identify NvDsMeta when iterating through
   * the buffer metadatas */
  if (!_dsmeta_quark)
    _dsmeta_quark = g_quark_from_static_string (NVDS_META_STRING);
}

/* Function called when a property of the element is set. Standard boilerplate.
 */
static void
gst_dsdemo_set_property (GObject * object, guint prop_id,
    const GValue * value, GParamSpec * pspec)
{
  GstDsDemo *dsdemo = GST_DSDEMO (object);
  switch (prop_id) {
    case PROP_UNIQUE_ID:
      dsdemo->unique_id = g_value_get_uint (value);
      break;
    case PROP_PROCESSING_WIDTH:
      dsdemo->processing_width = g_value_get_int (value);
      break;
    case PROP_PROCESSING_HEIGHT:
      dsdemo->processing_height = g_value_get_int (value);
      break;
    case PROP_PROCESS_FULL_FRAME:
      dsdemo->process_full_frame = g_value_get_boolean (value);
      break;
    case PROP_BLUR_OBJECTS:
      dsdemo->blur_objects = g_value_get_boolean (value);
      break;
    case PROP_GPU_DEVICE_ID:
      dsdemo->gpu_id = g_value_get_uint (value);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}

/* Function called when a property of the element is requested. Standard
 * boilerplate.
 */
static void
gst_dsdemo_get_property (GObject * object, guint prop_id,
    GValue * value, GParamSpec * pspec)
{
  GstDsDemo *dsdemo = GST_DSDEMO (object);

  switch (prop_id) {
    case PROP_UNIQUE_ID:
      g_value_set_uint (value, dsdemo->unique_id);
      break;
    case PROP_PROCESSING_WIDTH:
      g_value_set_int (value, dsdemo->processing_width);
      break;
    case PROP_PROCESSING_HEIGHT:
      g_value_set_int (value, dsdemo->processing_height);
      break;
    case PROP_PROCESS_FULL_FRAME:
      g_value_set_boolean (value, dsdemo->process_full_frame);
      break;
    case PROP_BLUR_OBJECTS:
      g_value_set_boolean (value, dsdemo->blur_objects);
      break;
    case PROP_GPU_DEVICE_ID:
      g_value_set_uint (value, dsdemo->gpu_id);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}

/**
 * Initialize all resources and start the output thread
 */
static gboolean
gst_dsdemo_start (GstBaseTransform * btrans)
{
  GstDsDemo *dsdemo = GST_DSDEMO (btrans);
  NvBufSurfaceCreateParams create_params;
  //DsExampleInitParams init_params =
  //    { dsdemo->processing_width, dsdemo->processing_height,
  //  dsdemo->process_full_frame
  //};

  GstQuery *queryparams = NULL;
  guint batch_size = 1;
  int val = -1;

  /* Algorithm specific initializations and resource allocation. */
  //dsdemo->dsdemolib_ctx = DsExampleCtxInit (&init_params);

  //GST_DEBUG_OBJECT (dsdemo, "ctx lib %p \n", dsdemo->dsdemolib_ctx);

  CHECK_CUDA_STATUS (cudaSetDevice (dsdemo->gpu_id),
      "Unable to set cuda device");

  cudaDeviceGetAttribute (&val, cudaDevAttrIntegrated, dsdemo->gpu_id);
  dsdemo->is_integrated = val;

  dsdemo->batch_size = 1;
  queryparams = gst_nvquery_batch_size_new ();
  if (gst_pad_peer_query (GST_BASE_TRANSFORM_SINK_PAD (btrans), queryparams)
      || gst_pad_peer_query (GST_BASE_TRANSFORM_SRC_PAD (btrans), queryparams)) {
    if (gst_nvquery_batch_size_parse (queryparams, &batch_size)) {
      dsdemo->batch_size = batch_size;
    }
  }
  GST_DEBUG_OBJECT (dsdemo, "Setting batch-size %d \n",
      dsdemo->batch_size);
  gst_query_unref (queryparams);

  CHECK_CUDA_STATUS (cudaStreamCreate (&dsdemo->cuda_stream),
      "Could not create cuda stream");

  if (dsdemo->inter_buf)
    NvBufSurfaceDestroy (dsdemo->inter_buf);
  dsdemo->inter_buf = NULL;

  /* An intermediate buffer for NV12/RGBA to BGR conversion  will be
   * required. Can be skipped if custom algorithm can work directly on NV12/RGBA. */
  create_params.gpuId  = dsdemo->gpu_id;
  create_params.width  = dsdemo->processing_width;
  create_params.height = dsdemo->processing_height;
  create_params.size = 0;
  create_params.colorFormat = NVBUF_COLOR_FORMAT_RGBA;
  create_params.layout = NVBUF_LAYOUT_PITCH;

  if(dsdemo->is_integrated) {
    create_params.memType = NVBUF_MEM_DEFAULT;
  }
  else {
    create_params.memType = NVBUF_MEM_CUDA_PINNED;
  }

  if (NvBufSurfaceCreate (&dsdemo->inter_buf, 1,
          &create_params) != 0) {
    GST_ERROR ("Error: Could not allocate internal buffer for dsdemo");
    goto error;
  }

  /* Create host memory for storing converted/scaled interleaved RGB data */
  CHECK_CUDA_STATUS (cudaMallocHost (&dsdemo->host_rgb_buf,
          dsdemo->processing_width * dsdemo->processing_height *
          RGB_BYTES_PER_PIXEL), "Could not allocate cuda host buffer");

  GST_DEBUG_OBJECT (dsdemo, "allocated cuda buffer %p \n",
      dsdemo->host_rgb_buf);

  return TRUE;
error:
  if (dsdemo->host_rgb_buf) {
    cudaFreeHost (dsdemo->host_rgb_buf);
    dsdemo->host_rgb_buf = NULL;
  }

  if (dsdemo->cuda_stream) {
    cudaStreamDestroy (dsdemo->cuda_stream);
    dsdemo->cuda_stream = NULL;
  }
  //if (dsdemo->dsdemolib_ctx)
  //  DsExampleCtxDeinit (dsdemo->dsdemolib_ctx);
  return FALSE;
}

/**
 * Stop the output thread and free up all the resources
 */
static gboolean
gst_dsdemo_stop (GstBaseTransform * btrans)
{
  GstDsDemo *dsdemo = GST_DSDEMO (btrans);

  if (dsdemo->inter_buf)
    NvBufSurfaceDestroy(dsdemo->inter_buf);
  dsdemo->inter_buf = NULL;

  if (dsdemo->cuda_stream)
    cudaStreamDestroy (dsdemo->cuda_stream);
  dsdemo->cuda_stream = NULL;

  if (dsdemo->host_rgb_buf) {
    cudaFreeHost (dsdemo->host_rgb_buf);
    dsdemo->host_rgb_buf = NULL;
  }

  GST_DEBUG_OBJECT (dsdemo, "deleted CV Mat \n");

  /* Deinit the algorithm library */
  //DsExampleCtxDeinit (dsdemo->dsdemolib_ctx);
  //dsdemo->dsdemolib_ctx = NULL;

  GST_DEBUG_OBJECT (dsdemo, "ctx lib released \n");

  return TRUE;
}

/**
 * Called when source / sink pad capabilities have been negotiated.
 */
static gboolean
gst_dsdemo_set_caps (GstBaseTransform * btrans, GstCaps * incaps,
    GstCaps * outcaps)
{
  GstDsDemo *dsdemo = GST_DSDEMO (btrans);
  /* Save the input video information, since this will be required later. */
  gst_video_info_from_caps (&dsdemo->video_info, incaps);

  return TRUE;

error:
  return FALSE;
}

/**
 * Called when element recieves an input buffer from upstream element.
 */
static GstFlowReturn
gst_dsdemo_transform_ip (GstBaseTransform * btrans, GstBuffer * inbuf)
{
  GstDsDemo *dsdemo = GST_DSDEMO (btrans);
  GstMapInfo in_map_info;
  GstFlowReturn flow_ret = GST_FLOW_ERROR;
  gdouble scale_ratio = 1.0;
  //DsExampleOutput *output;

  NvBufSurface *surface = NULL;
  NvDsBatchMeta *batch_meta = NULL;
  NvDsFrameMeta *frame_meta = NULL;
  NvDsMetaList * l_frame = NULL;
  guint i = 0;

  dsdemo->frame_num++;
  CHECK_CUDA_STATUS (cudaSetDevice (dsdemo->gpu_id),
      "Unable to set cuda device");
 
  printf("%s  frame_cnt : %ld\n", __FUNCTION__,  dsdemo->frame_num);
  
  memset (&in_map_info, 0, sizeof (in_map_info));
  if (!gst_buffer_map (inbuf, &in_map_info, GST_MAP_READ)) {
    g_print ("Error: Failed to map gst buffer\n");
    goto error;
  }

  nvds_set_input_system_timestamp (inbuf, GST_ELEMENT_NAME (dsdemo));
  surface = (NvBufSurface *) in_map_info.data;
  GST_DEBUG_OBJECT (dsdemo,
      "Processing Frame %" G_GUINT64_FORMAT " Surface %p\n",
      dsdemo->frame_num, surface);

  if (CHECK_NVDS_MEMORY_AND_GPUID (dsdemo, surface))
    goto error;

  batch_meta = gst_buffer_get_nvds_batch_meta (inbuf);
  if (batch_meta == nullptr) {
    GST_ELEMENT_ERROR (dsdemo, STREAM, FAILED,
        ("NvDsBatchMeta not found for input buffer."), (NULL));
    return GST_FLOW_ERROR;
  }

  flow_ret = GST_FLOW_OK;

error:

  nvds_set_output_system_timestamp (inbuf, GST_ELEMENT_NAME (dsdemo));
  gst_buffer_unmap (inbuf, &in_map_info);
  return flow_ret;
}

/**
 * Boiler plate for registering a plugin and an element.
 */
static gboolean
dsdemo_plugin_init (GstPlugin * plugin)
{
  GST_DEBUG_CATEGORY_INIT (gst_dsdemo_debug, "dsdemo", 0,
      "dsdemo plugin");

  return gst_element_register (plugin, "dsdemo", GST_RANK_PRIMARY,
      GST_TYPE_DSDEMO);
}

GST_PLUGIN_DEFINE (GST_VERSION_MAJOR,
    GST_VERSION_MINOR,
    nvds_demo,
    DESCRIPTION, 
    dsdemo_plugin_init, 
    "6.1", 
    LICENSE, 
    BINARY_PACKAGE, 
    URL)
