#include "background-filter.h"
#include <cstdint>
#define STB_IMAGE_IMPLEMENTATION
#include <onnxruntime_cxx_api.h>

#ifdef _WIN32
#include <wchar.h>
#endif // _WIN32

#include <opencv2/imgproc.hpp>
#include <graphics/vec2.h>

#include <memory>
#include <exception>
#include <mutex>
#include <regex>

#include <plugin-support.h>
#include "models/ModelSINET.h"
#include "models/ModelMediapipe.h"
#include "models/ModelSelfie.h"
#include "models/ModelRVM.h"
#include "models/ModelPPHumanSeg.h"
#include "models/ModelTCMonoDepth.h"
#include "models/ModelRMBG.h"
#include "FilterData.h"
#include "ort-utils/ort-session-utils.h"
#include "stb_image.h"
#include "obs-utils/obs-utils.h"
#include "consts.h"
#include "update-checker/update-checker.h"

struct background_removal_filter : public filter_data {
	bool enableThreshold = true;
	float threshold = 0.5f;
	cv::Scalar backgroundColor{0, 0, 0, 0};
	float contourFilter = 0.05f;
	float smoothContour = 0.5f;
	float feather = 0.0f;

	cv::Mat backgroundMask;
	cv::Mat lastBackgroundMask;
	cv::Mat lastImageBGRA;
	float temporalSmoothFactor = 0.0f;
	float imageSimilarityThreshold = 35.0f;
	bool enableImageSimilarity = true;
	int maskEveryXFrames = 1;
	int maskEveryXFramesCount = 0;
	int64_t blurBackground = 0;
	bool enableFocalBlur = false;
	float blurFocusPoint = 0.1f;
	float blurFocusDepth = 0.1f;
    std::string bgPath;
	gs_texture_t *bg_img = nullptr;
    obs_source_t *mediaSource;
    gs_texrender_t *videoTexrender;
    bool isVideo;
	std::string currentImagePath;
	float bg_offset_x = 0.0f;
    float bg_offset_y = 0.0f;
    float bg_scale_x = 1.0f;
    float bg_scale_y = 1.0f;

	gs_effect_t *effect;
	gs_effect_t *kawaseBlurEffect;

	std::mutex modelMutex;
};


/*  UTIL FUNCTIONS   */
static bool is_video_file(const std::string &path)
{
    const char *video_extensions[] = {".mp4", ".mov", ".avi", ".mkv", ".webm", ".flv", ".wmv"};
    std::string lower_path = path;
    std::transform(lower_path.begin(), lower_path.end(), lower_path.begin(), ::tolower);

    for (const char *ext : video_extensions) {
        if (lower_path.find(ext) != std::string::npos) {
            return true;
        }
    }
    return false;
}

static void create_media_source(struct background_removal_filter *tf)
{
    obs_log(LOG_INFO, "Entering create_media_source");

    if (tf->mediaSource) {
        obs_log(LOG_INFO, "Removing previous media source");
        obs_source_remove_active_child(tf->source, tf->mediaSource);
        obs_source_release(tf->mediaSource);
        tf->mediaSource = nullptr;
    }

    if (!tf->bgPath.empty() && tf->isVideo) {
        obs_log(LOG_INFO, "Creating new media source for: %s", tf->bgPath.c_str());
        obs_data_t *settings = obs_data_create();
        obs_data_set_string(settings, "local_file", tf->bgPath.c_str());
        obs_data_set_bool(settings, "looping", true);
        obs_data_set_bool(settings, "restart_on_activate", false);

        tf->mediaSource = obs_source_create_private("ffmpeg_source", "Background Video", settings);
        obs_data_release(settings);

        if (tf->mediaSource) {
            obs_log(LOG_INFO, "Media source created successfully: %p", (void*)tf->mediaSource);
            obs_source_add_active_child(tf->source, tf->mediaSource);
        } else {
            obs_log(LOG_ERROR, "Failed to create media source");
        }
    } else {
        obs_log(LOG_INFO, "No video background set or not a video file");
    }
}

// handle the enable/disable event
static void background_filter_enable_changed(void *data, calldata_t *calldata)
{
    struct background_removal_filter *tf = reinterpret_cast<background_removal_filter *>(data);
    bool enabled = calldata_bool(calldata, "enabled");
	obs_log(LOG_INFO, "Background filter enable changed: %d", enabled);
	obs_log(LOG_INFO, "Background filter isVideo: %d", tf->isVideo);
	obs_log(LOG_INFO, "Background media source: %p", (void*)tf->mediaSource);

	if (!enabled) {
		tf->isDisabled = true;
	}
    if (enabled) {
		tf->isDisabled = false;
    }
}



/********************** */


void background_removal_thread(void *data); // Forward declaration

const char *background_filter_getname(void *unused)
{
	UNUSED_PARAMETER(unused);
	return obs_module_text("BackgroundRemoval");
}

/**                   PROPERTIES                     */

static bool visible_on_bool(obs_properties_t *ppts, obs_data_t *settings,
			    const char *bool_prop, const char *prop_name)
{
	const bool enabled = obs_data_get_bool(settings, bool_prop);
	obs_property_t *p = obs_properties_get(ppts, prop_name);
	obs_property_set_visible(p, enabled);
	return true;
}

static bool enable_threshold_modified(obs_properties_t *ppts, obs_property_t *p,
				      obs_data_t *settings)
{
	UNUSED_PARAMETER(p);
	return visible_on_bool(ppts, settings, "enable_threshold",
			       "threshold_group");
}

static bool enable_focal_blur(obs_properties_t *ppts, obs_property_t *p,
			      obs_data_t *settings)
{
	UNUSED_PARAMETER(p);
	return visible_on_bool(ppts, settings, "enable_focal_blur",
			       "focal_blur_group");
}

static bool enable_image_similarity(obs_properties_t *ppts, obs_property_t *p,
				    obs_data_t *settings)
{
	UNUSED_PARAMETER(p);
	return visible_on_bool(ppts, settings, "enable_image_similarity",
			       "image_similarity_threshold");
}

static bool enable_advanced_settings(obs_properties_t *ppts, obs_property_t *p,
				     obs_data_t *settings)
{
	const bool enabled = obs_data_get_bool(settings, "advanced");
	p = obs_properties_get(ppts, "blur_background");
	obs_property_set_visible(p, true);

	for (const char *prop_name :
	     {"model_select", "useGPU", "mask_every_x_frames", "numThreads",
	      "enable_focal_blur", "enable_threshold", "threshold_group",
	      "focal_blur_group", "temporal_smooth_factor",
	      "image_similarity_threshold", "enable_image_similarity"}) {
		p = obs_properties_get(ppts, prop_name);
		obs_property_set_visible(p, enabled);
	}

	if (enabled) {
		enable_threshold_modified(ppts, p, settings);
		enable_focal_blur(ppts, p, settings);
		enable_image_similarity(ppts, p, settings);
	}

	return true;
}

obs_properties_t *background_filter_properties(void *data)
{
	obs_properties_t *props = obs_properties_create();

	obs_property_t *advanced = obs_properties_add_bool(
		props, "advanced", obs_module_text("Advanced"));

	// If advanced is selected show the advanced settings, otherwise hide them
	obs_property_set_modified_callback(advanced, enable_advanced_settings);

	/* Threshold props */
	obs_property_t *p_enable_threshold = obs_properties_add_bool(
		props, "enable_threshold", obs_module_text("EnableThreshold"));
	obs_property_set_modified_callback(p_enable_threshold,
					   enable_threshold_modified);
	
	obs_properties_add_path(
        props, "bg_path",
        obs_module_text("BG Image"),
        OBS_PATH_FILE,
        "Image/Video Files (*.png *.jpg *.jpeg *.bmp *.tga *.mp4 *.mov *.avi *.mkv);;All Files (*.*)",
    NULL);

	obs_properties_add_float_slider(props, "bg_offset_x", "Background Offset X", -1.0, 1.0, 0.01);
    obs_properties_add_float_slider(props, "bg_offset_y", "Background Offset Y", -1.0, 1.0, 0.01);
    obs_properties_add_float_slider(props, "bg_scale_x", "Background Scale X", 0.1, 5.0, 0.01);
    obs_properties_add_float_slider(props, "bg_scale_y", "Background Scale Y", 0.1, 5.0, 0.01);


	// Threshold props group
	obs_properties_t *threshold_props = obs_properties_create();

	obs_properties_add_float_slider(threshold_props, "threshold",
					obs_module_text("Threshold"), 0.0, 1.0,
					0.025);

	obs_properties_add_float_slider(
		threshold_props, "contour_filter",
		obs_module_text("ContourFilterPercentOfImage"), 0.0, 1.0,
		0.025);

	obs_properties_add_float_slider(threshold_props, "smooth_contour",
					obs_module_text("SmoothSilhouette"),
					0.0, 1.0, 0.05);

	obs_properties_add_float_slider(
		threshold_props, "feather",
		obs_module_text("FeatherBlendSilhouette"), 0.0, 1.0, 0.05);

	obs_properties_add_group(props, "threshold_group",
				 obs_module_text("ThresholdGroup"),
				 OBS_GROUP_NORMAL, threshold_props);

	/* GPU, CPU and performance Props */
	obs_property_t *p_use_gpu = obs_properties_add_list(
		props, "useGPU", obs_module_text("InferenceDevice"),
		OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);

	obs_property_list_add_string(p_use_gpu, obs_module_text("CPU"),
				     USEGPU_CPU);
#if defined(__linux__) && defined(__x86_64__)
	obs_property_list_add_string(p_use_gpu, obs_module_text("GPUTensorRT"),
				     USEGPU_TENSORRT);
	obs_property_list_add_string(p_use_gpu, obs_module_text("GPUCUDA"),
				     USEGPU_CUDA);
#endif
#if _WIN32
	obs_property_list_add_string(p_use_gpu, obs_module_text("GPUDirectML"),
				     USEGPU_DML);
#endif
#if defined(__APPLE__)
	obs_property_list_add_string(p_use_gpu, obs_module_text("CoreML"),
				     USEGPU_COREML);
#endif

	obs_properties_add_int(props, "mask_every_x_frames",
			       obs_module_text("CalculateMaskEveryXFrame"), 1,
			       300, 1);
	obs_properties_add_int_slider(props, "numThreads",
				      obs_module_text("NumThreads"), 0, 8, 1);


	/* Model selection Props */
	obs_property_t *p_model_select = obs_properties_add_list(
		props, "model_select", obs_module_text("SegmentationModel"),
		OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);

	obs_property_list_add_string(p_model_select, obs_module_text("SINet"),
				     MODEL_SINET);
	obs_property_list_add_string(
		p_model_select, obs_module_text("MediaPipe"), MODEL_MEDIAPIPE);
	obs_property_list_add_string(p_model_select,
				     obs_module_text("Selfie Segmentation"),
				     MODEL_SELFIE);
	obs_property_list_add_string(p_model_select,
				     obs_module_text("PPHumanSeg"),
				     MODEL_PPHUMANSEG);
	obs_property_list_add_string(p_model_select,
				     obs_module_text("Robust Video Matting"),
				     MODEL_RVM);
	obs_property_list_add_string(p_model_select,
				     obs_module_text("TCMonoDepth"),
				     MODEL_DEPTH_TCMONODEPTH);
	obs_property_list_add_string(p_model_select, obs_module_text("RMBG"),
				     MODEL_RMBG);

	obs_properties_add_float_slider(props, "temporal_smooth_factor",
					obs_module_text("TemporalSmoothFactor"),
					0.0, 1.0, 0.01);

	obs_property_t *p_enable_image_similarity = obs_properties_add_bool(
		props, "enable_image_similarity",
		obs_module_text("EnableImageSimilarity"));
	obs_property_set_modified_callback(p_enable_image_similarity,
					   enable_image_similarity);

	obs_properties_add_float_slider(
		props, "image_similarity_threshold",
		obs_module_text("ImageSimilarityThreshold"), 0.0, 100.0, 1.0);

	// /* Background Blur Props */
	// obs_properties_add_int_slider(
	// 	props, "blur_background",
	// 	obs_module_text("BlurBackgroundFactor0NoBlurUseColor"), 0, 20,
	// 	1);

	// obs_property_t *p_enable_focal_blur = obs_properties_add_bool(
	// 	props, "enable_focal_blur", obs_module_text("EnableFocalBlur"));
	// obs_property_set_modified_callback(p_enable_focal_blur,
	// 				   enable_focal_blur);

	// obs_properties_t *focal_blur_props = obs_properties_create();

	// obs_properties_add_float_slider(focal_blur_props, "blur_focus_point",
	// 				obs_module_text("BlurFocusPoint"), 0.0,
	// 				1.0, 0.05);
	// obs_properties_add_float_slider(focal_blur_props, "blur_focus_depth",
	// 				obs_module_text("BlurFocusDepth"), 0.0,
	// 				0.3, 0.02);

	// obs_properties_add_group(props, "focal_blur_group",
	// 			 obs_module_text("FocalBlurGroup"),
	// 			 OBS_GROUP_NORMAL, focal_blur_props);

	// Add a informative text about the plugin
	// replace the placeholder with the current version
	// use std::regex_replace instead of QString::arg because the latter doesn't work on Linux
	std::string basic_info = std::regex_replace(
		PLUGIN_INFO_TEMPLATE, std::regex("%1"), PLUGIN_VERSION);
	// // Check for update
	// if (get_latest_version() != nullptr) {
	// 	basic_info += std::regex_replace(
	// 		PLUGIN_INFO_TEMPLATE_UPDATE_AVAILABLE, std::regex("%1"),
	// 		get_latest_version());
	// }
	obs_properties_add_text(props, "info", basic_info.c_str(),
				OBS_TEXT_INFO);

	UNUSED_PARAMETER(data);
	return props;
}

void background_filter_defaults(obs_data_t *settings)
{
	obs_data_set_default_bool(settings, "advanced", false);
	obs_data_set_default_bool(settings, "enable_threshold", true);

	obs_data_set_default_double(settings, "bg_offset_x", 0.0);
    obs_data_set_default_double(settings, "bg_offset_y", 0.0);
    obs_data_set_default_double(settings, "bg_scale_x", 1.0);
    obs_data_set_default_double(settings, "bg_scale_y", 1.0);
	
	obs_data_set_default_double(settings, "threshold", 0.5);
	obs_data_set_default_double(settings, "contour_filter", 0.05);
	obs_data_set_default_double(settings, "smooth_contour", 0.5);
	obs_data_set_default_double(settings, "feather", 0.0);
#if _WIN32
	obs_data_set_default_string(settings, "useGPU", USEGPU_DML);
#elif defined(__APPLE__)
	obs_data_set_default_string(settings, "useGPU", USEGPU_CPU);
#else
	// Linux
	obs_data_set_default_string(settings, "useGPU", USEGPU_CPU);
#endif
	obs_data_set_default_string(settings, "model_select", MODEL_MEDIAPIPE);
	obs_data_set_default_int(settings, "mask_every_x_frames", 1);
	obs_data_set_default_int(settings, "blur_background", 0);
	obs_data_set_default_int(settings, "numThreads", 1);
	obs_data_set_default_bool(settings, "enable_focal_blur", false);
	obs_data_set_default_double(settings, "temporal_smooth_factor", 0.85);
	obs_data_set_default_double(settings, "image_similarity_threshold",
				    35.0);
	obs_data_set_default_bool(settings, "enable_image_similarity", true);
	obs_data_set_default_double(settings, "blur_focus_point", 0.1);
	obs_data_set_default_double(settings, "blur_focus_depth", 0.0);
}

void background_filter_update(void *data, obs_data_t *settings)
{
	obs_log(LOG_INFO, "Background filter updated");
	struct background_removal_filter *tf =
		reinterpret_cast<background_removal_filter *>(data);

	tf->isDisabled = true;

	tf->enableThreshold = obs_data_get_bool(settings, "enable_threshold");
	tf->threshold = (float)obs_data_get_double(settings, "threshold");

	tf->bg_offset_x = (float)obs_data_get_double(settings, "bg_offset_x");
    tf->bg_offset_y = (float)obs_data_get_double(settings, "bg_offset_y");
    tf->bg_scale_x = (float)obs_data_get_double(settings, "bg_scale_x");
    tf->bg_scale_y = (float)obs_data_get_double(settings, "bg_scale_y");

	tf->contourFilter =
		(float)obs_data_get_double(settings, "contour_filter");
	tf->smoothContour =
		(float)obs_data_get_double(settings, "smooth_contour");
	tf->feather = (float)obs_data_get_double(settings, "feather");
	tf->maskEveryXFrames =
		(int)obs_data_get_int(settings, "mask_every_x_frames");
	tf->maskEveryXFramesCount = (int)(0);
	// tf->blurBackground = obs_data_get_int(settings, "blur_background");
	// tf->blurBackground = obs_data_get_int(settings, "blur_background");
	// tf->enableFocalBlur =
	// 	(bool)obs_data_get_bool(settings, "enable_focal_blur");
	// tf->blurFocusPoint =
	// 	(float)obs_data_get_double(settings, "blur_focus_point");
	// tf->blurFocusDepth =
	// 	(float)obs_data_get_double(settings, "blur_focus_depth");
	tf->temporalSmoothFactor =
		(float)obs_data_get_double(settings, "temporal_smooth_factor");
	tf->imageSimilarityThreshold = (float)obs_data_get_double(
		settings, "image_similarity_threshold");
	tf->enableImageSimilarity =
		(bool)obs_data_get_bool(settings, "enable_image_similarity");
    const char* bgPathRaw = obs_data_get_string(settings, "bg_path");
    obs_log(LOG_INFO, "Background path: %s", bgPathRaw);
    if (bgPathRaw && tf->bgPath != bgPathRaw) {
        tf->bgPath = bgPathRaw;
        tf->isVideo = is_video_file(tf->bgPath);
        obs_log(LOG_INFO, "Background type updated. isVideo: %d", tf->isVideo);
        
        if (tf->isVideo) {
            obs_log(LOG_INFO, "Creating media source for video");
            create_media_source(tf);
            obs_log(LOG_INFO, "Media source created: %p", (void*)tf->mediaSource);
        } else {
            // Clean up video resources if switching to an image
            if (tf->mediaSource) {
                obs_log(LOG_INFO, "Cleaning up previous media source");
                obs_source_remove_active_child(tf->source, tf->mediaSource);
                obs_source_release(tf->mediaSource);
                tf->mediaSource = nullptr;
            }
            if (tf->videoTexrender) {
                obs_log(LOG_INFO, "Destroying previous video texrender");
                gs_texrender_destroy(tf->videoTexrender);
                tf->videoTexrender = nullptr;
            }
        }
    }

	const std::string newUseGpu = obs_data_get_string(settings, "useGPU");
	const std::string newModel =
		obs_data_get_string(settings, "model_select");
	const uint32_t newNumThreads =
		(uint32_t)obs_data_get_int(settings, "numThreads");

	if (tf->modelSelection.empty() || tf->modelSelection != newModel ||
	    tf->useGPU != newUseGpu || tf->numThreads != newNumThreads) {
		// lock modelMutex
		std::unique_lock<std::mutex> lock(tf->modelMutex);

		// Re-initialize model if it's not already the selected one or switching inference device
		tf->modelSelection = newModel;
		tf->useGPU = newUseGpu;
		tf->numThreads = newNumThreads;

		if (tf->modelSelection == MODEL_SINET) {
			tf->model.reset(new ModelSINET);
		}
		if (tf->modelSelection == MODEL_SELFIE) {
			tf->model.reset(new ModelSelfie);
		}
		if (tf->modelSelection == MODEL_MEDIAPIPE) {
			tf->model.reset(new ModelMediaPipe);
		}
		if (tf->modelSelection == MODEL_RVM) {
			tf->model.reset(new ModelRVM);
		}
		if (tf->modelSelection == MODEL_PPHUMANSEG) {
			tf->model.reset(new ModelPPHumanSeg);
		}
		if (tf->modelSelection == MODEL_DEPTH_TCMONODEPTH) {
			tf->model.reset(new ModelTCMonoDepth);
		}
		if (tf->modelSelection == MODEL_RMBG) {
			tf->model.reset(new ModelRMBG);
		}

		int ortSessionResult = createOrtSession(tf);
		if (ortSessionResult != OBS_BGREMOVAL_ORT_SESSION_SUCCESS) {
			obs_log(LOG_ERROR,
				"Failed to create ONNXRuntime session. Error code: %d",
				ortSessionResult);
			// disable filter
			tf->isDisabled = true;
			tf->model.reset();
			return;
		}
	}

	obs_enter_graphics();

	char *effect_path = obs_module_file(EFFECT_PATH);
	gs_effect_destroy(tf->effect);
	tf->effect = gs_effect_create_from_file(effect_path, NULL);
	bfree(effect_path);

	char *kawaseBlurEffectPath = obs_module_file(KAWASE_BLUR_EFFECT_PATH);
	gs_effect_destroy(tf->kawaseBlurEffect);
	tf->kawaseBlurEffect =
		gs_effect_create_from_file(kawaseBlurEffectPath, NULL);
	bfree(kawaseBlurEffectPath);

	obs_leave_graphics();

	// Log the currently selected options
	obs_log(LOG_INFO, "Background Removal Filter Options:");
	// name of the source that the filter is attached to
	obs_log(LOG_INFO, "  Source: %s", obs_source_get_name(tf->source));
	obs_log(LOG_INFO, "  Model: %s", tf->modelSelection.c_str());
	obs_log(LOG_INFO, "  Inference Device: %s", tf->useGPU.c_str());
	obs_log(LOG_INFO, "  Num Threads: %d", tf->numThreads);
	obs_log(LOG_INFO, "  Image Path %s", tf->bgPath.c_str());
	obs_log(LOG_INFO, "  Enable Threshold: %s",
		tf->enableThreshold ? "true" : "false");
	obs_log(LOG_INFO, "  Threshold: %f", tf->threshold);
	obs_log(LOG_INFO, "  Contour Filter: %f", tf->contourFilter);
	obs_log(LOG_INFO, "  Smooth Contour: %f", tf->smoothContour);
	obs_log(LOG_INFO, "  Feather: %f", tf->feather);
	obs_log(LOG_INFO, "  Mask Every X Frames: %d", tf->maskEveryXFrames);
	obs_log(LOG_INFO, "  Enable Image Similarity: %s",
		tf->enableImageSimilarity ? "true" : "false");
	obs_log(LOG_INFO, "  Image Similarity Threshold: %f",
		tf->imageSimilarityThreshold);
	obs_log(LOG_INFO, "  Blur Background: %d", tf->blurBackground);
	obs_log(LOG_INFO, "  Enable Focal Blur: %s",
		tf->enableFocalBlur ? "true" : "false");
	obs_log(LOG_INFO, "  Blur Focus Point: %f", tf->blurFocusPoint);
	obs_log(LOG_INFO, "  Blur Focus Depth: %f", tf->blurFocusDepth);
	obs_log(LOG_INFO, "  Disabled: %s", tf->isDisabled ? "true" : "false");
#ifdef _WIN32
	obs_log(LOG_INFO, "  Model file path: %S", tf->modelFilepath.c_str());
#else
	obs_log(LOG_INFO, "  Model file path: %s", tf->modelFilepath.c_str());
#endif

	// enable
	tf->isDisabled = false;
}

void background_filter_activate(void *data)
{
	obs_log(LOG_INFO, "Background filter activated");
	struct background_removal_filter *tf =
		reinterpret_cast<background_removal_filter *>(data);
	tf->isDisabled = false;
}

void background_filter_deactivate(void *data)
{
	obs_log(LOG_INFO, "Background filter deactivated");
	struct background_removal_filter *tf =
		reinterpret_cast<background_removal_filter *>(data);
	tf->isDisabled = true;
}

/**                   FILTER CORE                     */

void *background_filter_create(obs_data_t *settings, obs_source_t *source)
{
	obs_log(LOG_INFO, "Background filter created");
	void *data = bmalloc(sizeof(struct background_removal_filter));
	struct background_removal_filter *tf = new (data)
		background_removal_filter();

	tf->source = source;
	tf->texrender = gs_texrender_create(GS_BGRA, GS_ZS_NONE);

	std::string instanceName{"background-removal-inference"};
	tf->env.reset(new Ort::Env(OrtLoggingLevel::ORT_LOGGING_LEVEL_ERROR,
				   instanceName.c_str()));

	tf->mediaSource = nullptr;
    tf->videoTexrender = nullptr;
    tf->isVideo = false;

	tf->modelSelection = MODEL_MEDIAPIPE;
	background_filter_update(tf, settings);

	// Connect the enable signal handler
    signal_handler_t *handler = obs_source_get_signal_handler(source);
    signal_handler_connect(handler, "enable", background_filter_enable_changed, tf);

	

	return tf;
}

void background_filter_destroy(void *data)
{
	obs_log(LOG_INFO, "Background filter destroyed");

	struct background_removal_filter *tf =
		reinterpret_cast<background_removal_filter *>(data);

	if (tf) {
		tf->isDisabled = true;

		// Disconnect the signal handler
        signal_handler_t *handler = obs_source_get_signal_handler(tf->source);
        signal_handler_disconnect(handler, "enable", background_filter_enable_changed, tf);

		obs_enter_graphics();
		gs_texrender_destroy(tf->texrender);
		if (tf->stagesurface) {
			gs_stagesurface_destroy(tf->stagesurface);
		}
		if (tf->bg_img) {
			gs_texture_destroy(tf->bg_img);
		}
		if (tf->mediaSource) {
            obs_source_remove_active_child(tf->source, tf->mediaSource);
            obs_source_release(tf->mediaSource);
        }

        if (tf->videoTexrender) {
            gs_texrender_destroy(tf->videoTexrender);
        }

		gs_effect_destroy(tf->effect);
		gs_effect_destroy(tf->kawaseBlurEffect);
		obs_leave_graphics();
		tf->~background_removal_filter();
		bfree(tf);
	}
}

static void processImageForBackground(struct background_removal_filter *tf,
				      const cv::Mat &imageBGRA,
				      cv::Mat &backgroundMask)
{
	cv::Mat outputImage;
	if (!runFilterModelInference(tf, imageBGRA, outputImage)) {
		return;
	}
	// Assume outputImage is now a single channel, uint8 image with values between 0 and 255

	// If we have a threshold, apply it. Otherwise, just use the output image as the mask
	if (tf->enableThreshold) {
		// We need to make tf->threshold (float [0,1]) be in that range
		const uint8_t threshold_value =
			(uint8_t)(tf->threshold * 255.0f);
		backgroundMask = outputImage < threshold_value;
	} else {
		backgroundMask = 255 - outputImage;
	}
}

void background_filter_video_tick(void *data, float seconds)
{
	UNUSED_PARAMETER(seconds);

	struct background_removal_filter *tf =
		reinterpret_cast<background_removal_filter *>(data);

	if (tf->isDisabled) {
		return;
	}

	if (!obs_source_enabled(tf->source)) {
		return;
	}

	if (!tf->model) {
		obs_log(LOG_ERROR, "Model is not initialized");
		return;
	}

	cv::Mat imageBGRA;
	{
		std::unique_lock<std::mutex> lock(tf->inputBGRALock,
						  std::try_to_lock);
		if (!lock.owns_lock()) {
			// No data to process
			return;
		}
		if (tf->inputBGRA.empty()) {
			// No data to process
			return;
		}
		imageBGRA = tf->inputBGRA.clone();
	}

	if (tf->enableImageSimilarity) {
		if (!tf->lastImageBGRA.empty() && !imageBGRA.empty() &&
		    tf->lastImageBGRA.size() == imageBGRA.size()) {
			// calculate PSNR
			double psnr = cv::PSNR(tf->lastImageBGRA, imageBGRA);

			if (psnr > tf->imageSimilarityThreshold) {
				// The image is almost the same as the previous one. Skip processing.
				return;
			}
		}
		tf->lastImageBGRA = imageBGRA.clone();
	}

	if (tf->backgroundMask.empty()) {
		// First frame. Initialize the background mask.
		tf->backgroundMask =
			cv::Mat(imageBGRA.size(), CV_8UC1, cv::Scalar(255));
	}

	tf->maskEveryXFramesCount++;
	tf->maskEveryXFramesCount %= tf->maskEveryXFrames;

	try {
		if (tf->maskEveryXFramesCount != 0 &&
		    !tf->backgroundMask.empty()) {
			// We are skipping processing of the mask for this frame.
			// Get the background mask previously generated.
			; // Do nothing
		} else {
			cv::Mat backgroundMask;

			{
				std::unique_lock<std::mutex> lock(
					tf->modelMutex);
				// Process the image to find the mask.
				processImageForBackground(tf, imageBGRA,
							  backgroundMask);
			}

			if (backgroundMask.empty()) {
				// Something went wrong. Just use the previous mask.
				obs_log(LOG_WARNING,
					"Background mask is empty. This shouldn't happen. Using previous mask.");
				return;
			}

			// Temporal smoothing
			if (tf->temporalSmoothFactor > 0.0 &&
			    tf->temporalSmoothFactor < 1.0 &&
			    !tf->lastBackgroundMask.empty() &&
			    tf->lastBackgroundMask.size() ==
				    backgroundMask.size()) {

				float temporalSmoothFactor =
					tf->temporalSmoothFactor;
				if (tf->enableThreshold) {
					// The temporal smooth factor can't be smaller than the threshold
					temporalSmoothFactor =
						std::max(temporalSmoothFactor,
							 tf->threshold);
				}

				cv::addWeighted(backgroundMask,
						temporalSmoothFactor,
						tf->lastBackgroundMask,
						1.0 - temporalSmoothFactor, 0.0,
						backgroundMask);
			}

			tf->lastBackgroundMask = backgroundMask.clone();

			// Contour processing
			// Only applicable if we are thresholding (and get a binary image)
			if (tf->enableThreshold) {
				if (tf->contourFilter > 0.0 &&
				    tf->contourFilter < 1.0) {
					std::vector<std::vector<cv::Point>>
						contours;
					findContours(backgroundMask, contours,
						     cv::RETR_EXTERNAL,
						     cv::CHAIN_APPROX_SIMPLE);
					std::vector<std::vector<cv::Point>>
						filteredContours;
					const double contourSizeThreshold =
						(double)(backgroundMask.total()) *
						tf->contourFilter;
					for (auto &contour : contours) {
						if (cv::contourArea(contour) >
						    (double)contourSizeThreshold) {
							filteredContours
								.push_back(
									contour);
						}
					}
					backgroundMask.setTo(0);
					drawContours(backgroundMask,
						     filteredContours, -1,
						     cv::Scalar(255), -1);
				}

				if (tf->smoothContour > 0.0) {
					int k_size =
						(int)(3 +
						      11 * tf->smoothContour);
					k_size += k_size % 2 == 0 ? 1 : 0;
					cv::stackBlur(backgroundMask,
						      backgroundMask,
						      cv::Size(k_size, k_size));
				}

				// Resize the size of the mask back to the size of the original input.
				cv::resize(backgroundMask, backgroundMask,
					   imageBGRA.size());

				// Additional contour processing at full resolution
				if (tf->smoothContour > 0.0) {
					// If the mask was smoothed, apply a threshold to get a binary mask
					backgroundMask = backgroundMask > 128;
				}

				if (tf->feather > 0.0) {
					// Feather (blur) the mask
					int k_size = (int)(40 * tf->feather);
					k_size += k_size % 2 == 0 ? 1 : 0;
					cv::dilate(backgroundMask,
						   backgroundMask, cv::Mat(),
						   cv::Point(-1, -1),
						   k_size / 3);
					cv::boxFilter(
						backgroundMask, backgroundMask,
						tf->backgroundMask.depth(),
						cv::Size(k_size, k_size));
				}
			}

			// Save the mask for the next frame
			backgroundMask.copyTo(tf->backgroundMask);
		}
	} catch (const Ort::Exception &e) {
		obs_log(LOG_ERROR, "ONNXRuntime Exception: %s", e.what());
		// TODO: Fall back to CPU if it makes sense
	} catch (const std::exception &e) {
		obs_log(LOG_ERROR, "%s", e.what());
	}
}

static gs_texture_t *blur_background(struct background_removal_filter *tf,
				     uint32_t width, uint32_t height,
				     gs_texture_t *alphaTexture)
{
	if (tf->blurBackground == 0 || !tf->kawaseBlurEffect) {
		return nullptr;
	}
	gs_texture_t *blurredTexture =
		gs_texture_create(width, height, GS_BGRA, 1, nullptr, 0);
	gs_copy_texture(blurredTexture,
			gs_texrender_get_texture(tf->texrender));
	gs_eparam_t *image =
		gs_effect_get_param_by_name(tf->kawaseBlurEffect, "image");
	gs_eparam_t *focalmask =
		gs_effect_get_param_by_name(tf->kawaseBlurEffect, "focalmask");
	gs_eparam_t *xOffset =
		gs_effect_get_param_by_name(tf->kawaseBlurEffect, "xOffset");
	gs_eparam_t *yOffset =
		gs_effect_get_param_by_name(tf->kawaseBlurEffect, "yOffset");
	gs_eparam_t *blurIter =
		gs_effect_get_param_by_name(tf->kawaseBlurEffect, "blurIter");
	gs_eparam_t *blurTotal =
		gs_effect_get_param_by_name(tf->kawaseBlurEffect, "blurTotal");
	gs_eparam_t *blurFocusPointParam = gs_effect_get_param_by_name(
		tf->kawaseBlurEffect, "blurFocusPoint");
	gs_eparam_t *blurFocusDepthParam = gs_effect_get_param_by_name(
		tf->kawaseBlurEffect, "blurFocusDepth");

	for (int i = 0; i < (int)tf->blurBackground; i++) {
		gs_texrender_reset(tf->texrender);
		if (!gs_texrender_begin(tf->texrender, width, height)) {
			obs_log(LOG_INFO,
				"Could not open background blur texrender!");
			return blurredTexture;
		}

		gs_effect_set_texture(image, blurredTexture);
		gs_effect_set_texture(focalmask, alphaTexture);
		gs_effect_set_float(xOffset, ((float)i + 0.5f) / (float)width);
		gs_effect_set_float(yOffset, ((float)i + 0.5f) / (float)height);
		gs_effect_set_int(blurIter, i);
		gs_effect_set_int(blurTotal, (int)tf->blurBackground);
		gs_effect_set_float(blurFocusPointParam, tf->blurFocusPoint);
		gs_effect_set_float(blurFocusDepthParam, tf->blurFocusDepth);

		struct vec4 background;
		vec4_zero(&background);
		gs_clear(GS_CLEAR_COLOR, &background, 0.0f, 0);
		gs_ortho(0.0f, static_cast<float>(width), 0.0f,
			 static_cast<float>(height), -100.0f, 100.0f);
		gs_blend_state_push();
		gs_blend_function(GS_BLEND_ONE, GS_BLEND_ZERO);

		const char *blur_type = (tf->enableFocalBlur) ? "DrawFocalBlur"
							      : "Draw";

		while (gs_effect_loop(tf->kawaseBlurEffect, blur_type)) {
			gs_draw_sprite(blurredTexture, 0, width, height);
		}
		gs_blend_state_pop();
		gs_texrender_end(tf->texrender);
		gs_copy_texture(blurredTexture,
				gs_texrender_get_texture(tf->texrender));
	}
	return blurredTexture;
}

// Helper function
static uint32_t next_power_of_two(uint32_t v)
{
    v--;
    v |= v >> 1;
    v |= v >> 2;
    v |= v >> 4;
    v |= v >> 8;
    v |= v >> 16;
    v++;
    return v;
}
//TODO: Improve this logic, atm it just makes it go into a small window
gs_texture_t* create_texture_with_fallback(uint32_t img_width, uint32_t img_height, uint32_t texture_width, uint32_t texture_height, enum gs_color_format format, uint32_t levels, const uint8_t **data, uint32_t flags)
{
    // obs_log(LOG_INFO, "Attempting to create texture: Image(%dx%d), Desired(%dx%d), format: %d", 
    //         img_width, img_height, texture_width, texture_height, format);

    gs_texture_t* texture = nullptr;

    // Try creating the texture with desired dimensions
    try {
        texture = gs_texture_create(texture_width, texture_height, format, levels, nullptr, flags);
        if (texture) {
            // obs_log(LOG_INFO, "Texture created successfully with desired dimensions");
            // Resize the image data to fit the new texture
            gs_copy_texture_region(
                texture, 0, 0,
                gs_texture_create(img_width, img_height, format, levels, data, flags), 0, 0,
                img_width, img_height
            );
            return texture;
        }
    } catch (const std::exception& e) {
        obs_log(LOG_WARNING, "Exception caught while creating texture with desired dimensions: %s", e.what());
    }

    // Try creating the texture with original image dimensions
    try {
        texture = gs_texture_create(img_width, img_height, format, levels, data, flags);
        if (texture) {
            // obs_log(LOG_INFO, "Texture created successfully with original image dimensions");
            return texture;
        }
    } catch (const std::exception& e) {
        obs_log(LOG_WARNING, "Exception caught while creating texture with original dimensions: %s", e.what());
    }

    // Fallback options (if both above attempts fail)
    uint32_t fallback_widths[] = {
        next_power_of_two(texture_width),
        texture_width / 2,
        next_power_of_two(img_width),
        img_width / 2,
        1
    };
    uint32_t fallback_heights[] = {
        next_power_of_two(texture_height),
        texture_height / 2,
        next_power_of_two(img_height),
        img_height / 2,
        1
    };

    for (size_t i = 0; i < sizeof(fallback_widths) / sizeof(fallback_widths[0]); ++i) {
        try {
            texture = gs_texture_create(fallback_widths[i], fallback_heights[i], format, levels, nullptr, flags);
            if (texture) {
                // obs_log(LOG_INFO, "Texture created successfully with fallback dimensions: %dx%d", 
                //         fallback_widths[i], fallback_heights[i]);
                // Resize the image data to fit the new texture
                gs_copy_texture_region(
                    texture, 0, 0,
                    gs_texture_create(img_width, img_height, format, levels, data, flags), 0, 0,
                    std::min(img_width, fallback_widths[i]),
                    std::min(img_height, fallback_heights[i])
                );
                return texture;
            }
        } catch (const std::exception& e) {
            obs_log(LOG_WARNING, "Exception caught while creating texture with fallback dimensions: %s", e.what());
        }
    }

    obs_log(LOG_ERROR, "All texture creation attempts failed.");
    return nullptr;
}

gs_texture_t* load_image_as_texture(const char* file_path, uint32_t texture_width, uint32_t texture_height)
{
	UNUSED_PARAMETER(texture_width);
	UNUSED_PARAMETER(texture_height);
    obs_log(LOG_INFO, "Attempting to load image as texture: %s", file_path);

    int width, height, channels;
    unsigned char* image_data = stbi_load(file_path, &width, &height, &channels, STBI_rgb_alpha);
    
    if (!image_data) {
        obs_log(LOG_ERROR, "Failed to load image: %s", file_path);
        return nullptr;
    }

    obs_log(LOG_INFO, "Image loaded successfully. Dimensions: %dx%d, Channels: %d", width, height, channels);

	//For now just using the image dimensions as the texture dimensions - the actaul dimensions are not that important now I have scaling in place
	gs_texture_t* texture = create_texture_with_fallback(static_cast<uint32_t>(width), static_cast<uint32_t>(height), static_cast<uint32_t>(width), static_cast<uint32_t>(height), GS_RGBA, 1, (const uint8_t**)&image_data, 0);

    stbi_image_free(image_data);

    if (!texture) {
        obs_log(LOG_ERROR, "Failed to create texture from image: %s", file_path);
        return nullptr;
    }

    obs_log(LOG_INFO, "Texture created successfully. Dimensions: %dx%d", gs_texture_get_width(texture), gs_texture_get_height(texture));

    return texture;
}

static gs_texture_t *get_bg_img(struct background_removal_filter *tf,
                                uint32_t width, uint32_t height)
{
    // Check if the path has changed
    if (tf->currentImagePath != tf->bgPath) {
		obs_log(LOG_INFO, "New texture being made");
        // Path has changed, clean up old texture if it exists
        if (tf->bg_img) {
            gs_texture_destroy(tf->bg_img);
            tf->bg_img = nullptr;
        }
        // Update the current path
        tf->currentImagePath = tf->bgPath;
    }

    // If the texture is already loaded, return it
    if (tf->bg_img) {
		obs_log(LOG_INFO, "Texture already loaded");
        return tf->bg_img;
    }

    // Check if the path is valid (not empty and contains non-whitespace characters)
    if (!tf->currentImagePath.empty() && 
        std::any_of(tf->currentImagePath.begin(), tf->currentImagePath.end(), 
                    [](unsigned char ch) { return !std::isspace(ch); })) {
        obs_log(LOG_INFO, "Loading new texture from: %s", tf->currentImagePath.c_str());
        try {
            gs_texture_t* loaded_texture = load_image_as_texture(tf->currentImagePath.c_str(), width, height);
            if (loaded_texture) {
                // If we had a previous texture, destroy it
                if (tf->bg_img) {
                    gs_texture_destroy(tf->bg_img);
                }
                tf->bg_img = loaded_texture;
                obs_log(LOG_INFO, "Texture loaded successfully");
                return tf->bg_img;
            } else {
                obs_log(LOG_ERROR, "Failed to load texture");
            }
        } catch (const std::exception& e) {
            obs_log(LOG_ERROR, "Exception while loading texture: %s", e.what());
        }
    }
	obs_log(LOG_ERROR, "Texture failed to load");

    // If we get here, either the path was invalid or loading failed
    return nullptr;
}


static gs_texture_t *get_bg_texture(struct background_removal_filter *tf, uint32_t width, uint32_t height)
{
    // obs_log(LOG_INFO, "Entering get_bg_texture. isVideo: %d, mediaSource: %p", tf->isVideo, (void*)tf->mediaSource);

    if (tf->isVideo && tf->mediaSource) {
        // obs_log(LOG_INFO, "Processing video source");

        if (!tf->videoTexrender) {
            tf->videoTexrender = gs_texrender_create(GS_RGBA, GS_ZS_NONE);
            // obs_log(LOG_INFO, "Created new videoTexrender: %p", (void*)tf->videoTexrender);
        }

        uint32_t source_cx = obs_source_get_width(tf->mediaSource);
        uint32_t source_cy = obs_source_get_height(tf->mediaSource);

        // obs_log(LOG_INFO, "Source dimensions: %ux%u", source_cx, source_cy);

        if (source_cx == 0 || source_cy == 0) {
            obs_log(LOG_ERROR, "Invalid source dimensions");
            return nullptr;
        }

        gs_texrender_reset(tf->videoTexrender);
        // obs_log(LOG_INFO, "Attempting to begin texrender");
        if (gs_texrender_begin(tf->videoTexrender, source_cx, source_cy)) {
            // obs_log(LOG_INFO, "Texrender begin successful");

            struct vec4 background;
            vec4_zero(&background);

            gs_clear(GS_CLEAR_COLOR, &background, 0.0f, 0);
            gs_ortho(0.0f, (float)source_cx, 0.0f, (float)source_cy, -100.0f, 100.0f);

            gs_blend_state_push();
            gs_blend_function(GS_BLEND_ONE, GS_BLEND_ZERO);

            obs_source_video_render(tf->mediaSource);

            gs_blend_state_pop();
            gs_texrender_end(tf->videoTexrender);

            // obs_log(LOG_INFO, "Video rendered successfully");
            return gs_texrender_get_texture(tf->videoTexrender);
        } else {
            obs_log(LOG_ERROR, "Failed to begin texrender");
        }
    } else if (!tf->isVideo) {
        // obs_log(LOG_INFO, "Processing static image");
        return get_bg_img(tf, width, height);
    }

    obs_log(LOG_ERROR, "Failed to get background texture");
    return nullptr;
}

void background_filter_video_render(void *data, gs_effect_t *_effect)
{
	UNUSED_PARAMETER(_effect);

	struct background_removal_filter *tf =
		reinterpret_cast<background_removal_filter *>(data);

	if (tf->isDisabled) {
		if (tf->source) {
			obs_source_skip_video_filter(tf->source);
		}
		return;
	}

	uint32_t width, height;
	if (!getRGBAFromStageSurface(tf, width, height)) {
		if (tf->source) {
			obs_source_skip_video_filter(tf->source);
		}
		return;
	}

	if (!tf->effect) {
		// Effect failed to load, skip rendering
		if (tf->source) {
			obs_source_skip_video_filter(tf->source);
		}
		return;
	}

	gs_texture_t *alphaTexture = nullptr;
	{
		std::lock_guard<std::mutex> lock(tf->outputLock);
		alphaTexture = gs_texture_create(
			static_cast<uint32_t>(tf->backgroundMask.cols),
			static_cast<uint32_t>(tf->backgroundMask.rows), GS_R8,
			1, (const uint8_t **)&tf->backgroundMask.data, 0);
		if (!alphaTexture) {
			obs_log(LOG_ERROR, "Failed to create alpha texture");
			if (tf->source) {
				obs_source_skip_video_filter(tf->source);
			}
			return;
		}
	}

    gs_texture_t *bgTexture = nullptr;
    if (!tf->bgPath.empty()) {
        bgTexture = get_bg_texture(tf, width, height);
    }

	if (!bgTexture) {
		// No background texture, skip rendering
		if (tf->source) {
			obs_source_skip_video_filter(tf->source);
		}
		gs_texture_destroy(alphaTexture);
		return;
	}


	// Output the masked image
	gs_texture_t *blurredTexture =
		blur_background(tf, width, height, alphaTexture);

	if (!obs_source_process_filter_begin(tf->source, GS_RGBA,
					     OBS_ALLOW_DIRECT_RENDERING)) {
		if (tf->source) {
			obs_source_skip_video_filter(tf->source);
		}
		gs_texture_destroy(alphaTexture);
		gs_texture_destroy(blurredTexture);
		return;
	}

	gs_eparam_t *alphamask =
		gs_effect_get_param_by_name(tf->effect, "alphamask");
	gs_eparam_t *blurredBackground =
		gs_effect_get_param_by_name(tf->effect, "blurredBackground");
	gs_eparam_t *bgimageParam =
		gs_effect_get_param_by_name(tf->effect, "bgImage");

	gs_effect_set_texture(alphamask, alphaTexture);

	if (tf->blurBackground > 0) {
		gs_effect_set_texture(blurredBackground, blurredTexture);
	}

	if (bgTexture) {
		gs_effect_set_texture(bgimageParam, bgTexture);
	}

    
    vec2 bgImageOffset;
    vec2_set(&bgImageOffset, tf->bg_offset_x, tf->bg_offset_y);
    gs_effect_set_vec2(gs_effect_get_param_by_name(tf->effect, "bgImageOffset"), &bgImageOffset);

    vec2 bgImageScale;
    vec2_set(&bgImageScale, tf->bg_scale_x, tf->bg_scale_y);
    gs_effect_set_vec2(gs_effect_get_param_by_name(tf->effect, "bgImageScale"), &bgImageScale);

	gs_blend_state_push();
	gs_reset_blend_state();

	const char *techName;
	if (bgTexture) {
		techName = "DrawWithBGImage";
	} else {
		//Skip this frame
		obs_source_skip_video_filter(tf->source);
	}

	obs_source_process_filter_tech_end(tf->source, tf->effect, 0, 0,
					   techName);

	gs_blend_state_pop();
	//don't destroy bgimage, it will be reused
	gs_texture_destroy(alphaTexture);
	gs_texture_destroy(blurredTexture);
}
