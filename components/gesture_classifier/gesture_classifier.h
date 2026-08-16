/*
 * gesture_classifier.h - lightweight gesture classifier for ESP32 (pure C, no external deps).
 *
 * Equivalent re-implementation of a 3-class gesture recognition pipeline:
 *   detrend (per-axis mean removal)
 *   -> STFT (2 Hann windows x 64 samples, 64-pt real FFT, magnitude bins 1..32)
 *   -> feature interleave (feature-major, channels interleaved)
 *   -> linear one-vs-rest SVM + softmax (beta scaled)
 *
 * All buffers are float32; the model lives in gesture_classifier_model.h.
 */
#ifndef GESTURE_CLASSIFIER_H
#define GESTURE_CLASSIFIER_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* State / return codes */
enum gc_state {
    GC_OK = 0,
    GC_ERROR = 1,
    GC_NOT_INITIALIZED = 2,
    GC_INVALID_PARAM = 3,
    GC_NOT_SUPPORTED = 4
};

/**
 * @brief  Initialise the classifier (loads/validates the embedded model).
 * @return GC_OK on success, error code otherwise.
 */
enum gc_state gc_init(void);

/**
 * @brief  Classify one input window.
 * @param  in             [in]   Pointer to input signal, interleaved samples:
 *                               size = gc_get_input_signal_size() * gc_get_axis_number()
 *                               (100 samples x 3 axes, layout [ax0,ay0,az0, ax1,ay1,az1, ...]).
 * @param  probabilities  [out]  Output per-class probabilities (size gc_get_number_of_classes()).
 * @param  id_class       [out]  Predicted class id in [0, gc_get_number_of_classes()-1].
 * @return GC_OK on success, error code otherwise.
 */
enum gc_state gc_classification(float *in, float *probabilities, int *id_class);

/**
 * @brief  Extract the 192-dim feature vector (useful for debugging / logging).
 * @param  in        [in]  Input signal, same layout as gc_classification().
 * @param  features  [out] Output features (size gc_get_feature_size()).
 * @return GC_OK on success, error code otherwise.
 */
enum gc_state gc_extract_features(float *in, float *features);

/**
 * @brief  Number of samples per axis.
 */
int gc_get_input_signal_size(void);

/**
 * @brief  Number of input axes / channels.
 */
int gc_get_axis_number(void);

/**
 * @brief  Number of classes in the model.
 */
int gc_get_number_of_classes(void);

/**
 * @brief  Feature vector size (extracted features per window).
 */
int gc_get_feature_size(void);

/**
 * @brief  Class name for a given class id, or NULL if invalid.
 */
const char *gc_get_class_name(int id_class);

#ifdef __cplusplus
}
#endif

#endif /* GESTURE_CLASSIFIER_H */