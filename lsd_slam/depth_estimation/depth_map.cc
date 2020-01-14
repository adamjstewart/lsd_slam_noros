/**
* This file is part of LSD-SLAM.
*
* Copyright 2013 Jakob Engel <engelj at in dot tum dot de> (Technical University of Munich)
* For more information see <http://vision.in.tum.de/lsdslam>
*
* LSD-SLAM is free software: you can redistribute it and/or modify
* it under the terms of the GNU General Public License as published by
* the Free Software Foundation, either version 3 of the License, or
* (at your option) any later version.
*
* LSD-SLAM is distributed in the hope that it will be useful,
* but WITHOUT ANY WARRANTY; without even the implied warranty of
* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
* GNU General Public License for more details.
*
* You should have received a copy of the GNU General Public License
* along with LSD-SLAM. If not, see <http://www.gnu.org/licenses/>.
*/

#include "depth_estimation/depth_map.h"

#include <chrono>
#include <stdio.h>
#include <fstream>
#include <iostream>
#include <cmath>
#include <opencv2/imgproc/imgproc.hpp>
#include <Eigen/Dense>

#include "util/settings.h"
#include "depth_estimation/depth_map_pixel_hypothesis.h"
#include "model/frame.h"
#include "util/global_funcs.h"
#include "io_wrapper/image_display.h"
#include "global_mapping/key_frame_graph.h"
#include "projection.h"
#include "math.h"


float calc_grad_along_line(const Eigen::VectorXf &intensities,
                           const float interval) {
    float grad_along_line = 0;
    for (int i = 0; i < intensities.size() - 1; i++) {
        float d = intensities[i+1] - intensities[i];
        grad_along_line += d * d;
    }

    return grad_along_line / (interval * interval);
}

bool is_in_image_range(const Eigen::Vector2f &keypoint,
                       const Eigen::Vector2i &image_size,
                       const int padding) {
    return (padding <= keypoint[0] &&
            padding <= keypoint[1] &&
            keypoint[0] <= image_size[0] - 1 - padding &&
            keypoint[1] <= image_size[1] - 1 - padding);
}


namespace lsd_slam {

float calc_geometric_disparity_error(
    const Eigen::Vector2f &interpolated_gradient,
    const Eigen::Vector2f &epipolar_direction,
    const float initialTrackedResidual) {
    // calculate error from photometric noise
    float trackingErrorFac = 0.25 * (1+initialTrackedResidual);

    // calculate error from geometric noise (wrong camera pose / calibration)
    const float p = epipolar_direction.dot(interpolated_gradient) + DIVISION_EPS;
    const float n = interpolated_gradient.squaredNorm();
    return trackingErrorFac * trackingErrorFac * n / (p * p);
}


DepthMap::DepthMap(int w, int h, const Eigen::Matrix3f& K)
{
    width = w;
    height = h;

    activeKeyFrame = 0;
    activeKeyFrameIsReactivated = false;
    otherDepthMap = new DepthMapPixelHypothesis[width*height];
    currentDepthMap = new DepthMapPixelHypothesis[width*height];

    validityIntegralBuffer = new int[width*height];

    debugImageHypothesisHandling = cv::Mat(h,w, CV_8UC3);
    debugImageHypothesisPropagation = cv::Mat(h,w, CV_8UC3);
    debugImageStereoLines = cv::Mat(h,w, CV_8UC3);
    debugImageDepth = cv::Mat(h,w, CV_8UC3);


    this->K = K;
    fx = K(0,0);
    fy = K(1,1);
    cx = K(0,2);
    cy = K(1,2);

    KInv = K.inverse();
    fxi = KInv(0,0);
    fyi = KInv(1,1);
    cxi = KInv(0,2);
    cyi = KInv(1,2);

    reset();

    msUpdate =  msCreate =  msFinalize = 0;
    msObserve =  msRegularize =  msPropagate =  msFillHoles =  msSetDepth = 0;
    // gettimeofday(&lastHzUpdate, NULL);
    lastHzUpdate = std::chrono::high_resolution_clock::now();
    nUpdate = nCreate = nFinalize = 0;
    nObserve = nRegularize = nPropagate = nFillHoles = nSetDepth = 0;
    nAvgUpdate = nAvgCreate = nAvgFinalize = 0;
    nAvgObserve = nAvgRegularize = nAvgPropagate = nAvgFillHoles = nAvgSetDepth =
                                       0;
}

DepthMap::~DepthMap()
{
    if(activeKeyFrame != 0)
        activeKeyFramelock.unlock();

    debugImageHypothesisHandling.release();
    debugImageHypothesisPropagation.release();
    debugImageStereoLines.release();
    debugImageDepth.release();

    delete[] otherDepthMap;
    delete[] currentDepthMap;

    delete[] validityIntegralBuffer;
}


void DepthMap::reset()
{
    for(DepthMapPixelHypothesis* pt = otherDepthMap+width*height-1;
            pt >= otherDepthMap; pt--)
        pt->isValid = false;
    for(DepthMapPixelHypothesis* pt = currentDepthMap+width*height-1;
            pt >= currentDepthMap; pt--)
        pt->isValid = false;
}


void DepthMap::observeDepthRow(int yMin, int yMax) {
    const float* keyFrameMaxGradBuf = activeKeyFrame->maxGradients(0);

    int successes = 0;

    for(int y=yMin; y<yMax; y++)
        for(int x=3; x<width-3; x++) {
            int idx = x+y*width;
            DepthMapPixelHypothesis* target = currentDepthMap+idx;
            bool hasHypothesis = target->isValid;

            // ======== 1. check absolute grad =========
            if(hasHypothesis &&
               keyFrameMaxGradBuf[idx] < MIN_ABS_GRAD_DECREASE) {
                target->isValid = false;
                continue;
            }

            if(keyFrameMaxGradBuf[idx] < MIN_ABS_GRAD_CREATE ||
               target->blacklisted < MIN_BLACKLIST)
                continue;

            const Eigen::Vector2i keyframe_coordinate(x, y);

            bool success;
            if(!hasHypothesis)
                success = observeDepthCreate(keyframe_coordinate, idx);
            else
                success = observeDepthUpdate(keyframe_coordinate, idx,
                                             keyFrameMaxGradBuf);

            if(success)
                successes++;
        }


}
void DepthMap::observeDepth()
{
    threadReducer.reduce(boost::bind(&DepthMap::observeDepthRow, this, _1, _2),
                         3, height-3, 10);
}


Eigen::Vector2f compute_image_gradient(const float* image,
                                       const int image_width,
                                       const Eigen::Vector2i &coordinate) {
    int x = coordinate[0];
    int y = coordinate[1];

    int idx = x+y*image_width;

    // ===== check epl-grad magnitude ======
    const float gx = image[idx+1] - image[idx-1];
    const float gy = image[idx+image_width] - image[idx-image_width];
    const Eigen::Vector2f grad(gx, gy);
    return grad;
}


bool DepthMap::makeAndCheckEPL(const Eigen::Vector2i &keyframe_coordinate,
                               const Eigen::Vector3f &thisToOther_t,
                               Eigen::Vector2f &pep) {

    // ======= make epl ========
    // calculate the plane spanned by the two camera centers and the point (x,y,1)
    // intersect it with the keyframe's image plane (at depth=1)
    const Eigen::Matrix3f &K = create_intrinsic_matrix(fx, fy, cx, cy);

    const Eigen::Vector2f epipolar_line = thisToOther_t[2] * (
        keyframe_coordinate.cast<float>() - perspective_projection(thisToOther_t, K)
    );

    // ======== check epl length =========
    const float epipolar_length_squared = epipolar_line.squaredNorm();

    if(epipolar_length_squared < MIN_EPL_LENGTH_SQUARED) {
        // stats->num_observe_skipped_small_epl++;
        return false;
    }
    const Eigen::Vector2f &grad = compute_image_gradient(
        activeKeyFrameImageData, width, keyframe_coordinate);

    const float grad_projected = grad.dot(epipolar_line);
    // square and norm with epl-length
    const float eplGradSquared = grad_projected * grad_projected / epipolar_length_squared;

    // FIXME is this condition really necessary?
    if(eplGradSquared < MIN_EPL_GRAD_SQUARED) {
        // stats->num_observe_skipped_small_epl_grad++;
        return false;
    }

    // ===== check epl-grad angle ======
    if(cosine_angle_squared(epipolar_line, grad) < MIN_EPL_ANGLE_SQUARED) {
        // stats->num_observe_skipped_small_epl_angle++;
        return false;
    }

    // ===== DONE - return "normalized" epl =====
    pep = epipolar_line / sqrt(epipolar_length_squared);

    return true;
}


bool DepthMap::observeDepthCreate(const Eigen::Vector2i &keyframe_coordinate,
                                  const int &idx)
{
    DepthMapPixelHypothesis* target = currentDepthMap+idx;

    Frame* refFrame = activeKeyFrameIsReactivated ? newest_referenceFrame :
                      oldest_referenceFrame;

    const int x = keyframe_coordinate[0];
    const int y = keyframe_coordinate[1];
    if(refFrame->getTrackingParent() == activeKeyFrame)
    {
        bool* wasGoodDuringTracking = refFrame->refPixelWasGoodNoCreate();
        if(wasGoodDuringTracking != 0 &&
           !wasGoodDuringTracking[(x >> SE3TRACKING_MIN_LEVEL) +
                                  (width >> SE3TRACKING_MIN_LEVEL) *
                                  (y >> SE3TRACKING_MIN_LEVEL)])
        {
            if(plotStereoImages)
                debugImageHypothesisHandling.at<cv::Vec3b>(y, x) = cv::Vec3b(255,0,
                        0); // BLUE for SKIPPED NOT GOOD TRACKED
            return false;
        }
    }


    // stats->num_observe_create_attempted++;

    float result_idepth, result_var, result_eplLength;
    float error = doLineStereo(
                      keyframe_coordinate, 0.0f, 1.0f, 1.0f/MIN_DEPTH,
                      refFrame, refFrame->image(0),
                      result_idepth, result_var, result_eplLength);

    if(error == -3 || error == -2)
    {
        target->blacklisted--;
        // stats->num_observe_blacklisted++;
    }

    if(error < 0 || result_var > MAX_VAR)
        return false;

    result_idepth = UNZERO(result_idepth);

    // add hypothesis
    *target = DepthMapPixelHypothesis(
                  result_idepth,
                  result_var,
                  VALIDITY_COUNTER_INITIAL_OBSERVE);

    if(plotStereoImages)
        debugImageHypothesisHandling.at<cv::Vec3b>(y, x) = cv::Vec3b(255,255,
                255); // white for GOT CREATED

    // stats->num_observe_created++;

    return true;
}

bool DepthMap::observeDepthUpdate(const Eigen::Vector2i &keyframe_coordinate,
                                  const int &idx,
                                  const float* keyFrameMaxGradBuf)
{
    DepthMapPixelHypothesis* target = currentDepthMap+idx;
    Frame* refFrame;

    const int x = keyframe_coordinate[0];
    const int y = keyframe_coordinate[1];
    if(!activeKeyFrameIsReactivated) {
        if((int)target->nextStereoFrameMinID - referenceFrameByID_offset >=
                (int)referenceFrameByID.size()) {
            if(plotStereoImages) {
                // GREEN FOR skip
                debugImageHypothesisHandling.at<cv::Vec3b>(y, x) = cv::Vec3b(0,255, 0);
            }

            // stats->num_observe_skip_alreadyGood++;
            return false;
        }

        if((int)target->nextStereoFrameMinID - referenceFrameByID_offset < 0)
            refFrame = oldest_referenceFrame;
        else
            refFrame = referenceFrameByID[(int)target->nextStereoFrameMinID -
                                          referenceFrameByID_offset];
    }
    else
        refFrame = newest_referenceFrame;


    if(refFrame->getTrackingParent() == activeKeyFrame)
    {
        bool* wasGoodDuringTracking = refFrame->refPixelWasGoodNoCreate();
        if(wasGoodDuringTracking != 0 &&
           !wasGoodDuringTracking[(x >> SE3TRACKING_MIN_LEVEL) +
                                  (width >> SE3TRACKING_MIN_LEVEL) *
                                  (y >> SE3TRACKING_MIN_LEVEL)])
        {
            if(plotStereoImages)
                // BLUE for SKIPPED NOT GOOD TRACKED
                debugImageHypothesisHandling.at<cv::Vec3b>(y, x) = cv::Vec3b(255, 0, 0);
            return false;
        }
    }

    // which exact point to track, and where from.
    float sv = sqrt(target->idepth_var_smoothed);
    float min_idepth = target->idepth_smoothed - sv*STEREO_EPL_VAR_FAC;
    float max_idepth = target->idepth_smoothed + sv*STEREO_EPL_VAR_FAC;
    if(min_idepth < 0) min_idepth = 0;
    if(max_idepth > 1/MIN_DEPTH) max_idepth = 1/MIN_DEPTH;

    // stats->num_observe_update_attempted++;

    float result_idepth, result_var, result_eplLength;

    float error = doLineStereo(
                      keyframe_coordinate, min_idepth,
                      target->idepth_smoothed, max_idepth,
                      refFrame, refFrame->image(0),
                      result_idepth, result_var, result_eplLength);

    float diff = result_idepth - target->idepth_smoothed;
    if(error == -5) {
        return false;
    }

    // if oob: (really out of bounds)
    if(error == -1)
    {
        // do nothing, pixel got oob, but is still in bounds in original. I will want to try again.
        // stats->num_observe_skip_oob++;

        if(plotStereoImages)
            debugImageHypothesisHandling.at<cv::Vec3b>(y, x) = cv::Vec3b(0,0,
                    255);	// RED FOR OOB
        return false;
    }

    // if just not good for stereo (e.g. some inf / nan occured; has inconsistent minimum; ..)
    else if(error == -2)
    {
        // stats->num_observe_skip_fail++;

        if(plotStereoImages)
            debugImageHypothesisHandling.at<cv::Vec3b>(y, x) = cv::Vec3b(255,0,
                    255);	// PURPLE FOR NON-GOOD


        target->validity_counter -= VALIDITY_COUNTER_DEC;
        if(target->validity_counter < 0) target->validity_counter = 0;


        target->nextStereoFrameMinID = 0;

        target->idepth_var *= FAIL_VAR_INC_FAC;
        if(target->idepth_var > MAX_VAR)
        {
            target->isValid = false;
            target->blacklisted--;
        }
        return false;
    }

    // if not found (error too high)
    else if(error == -3)
    {
        // stats->num_observe_notfound++;
        if(plotStereoImages)
            debugImageHypothesisHandling.at<cv::Vec3b>(y, x) = cv::Vec3b(0,0,
                    0);	// BLACK FOR big not-found


        return false;
    }

    else if(error == -4)
    {
        if(plotStereoImages)
            debugImageHypothesisHandling.at<cv::Vec3b>(y, x) = cv::Vec3b(0,0,
                    0);	// BLACK FOR big arithmetic error

        return false;
    }

    // if inconsistent
    else if(DIFF_FAC_OBSERVE*diff*diff > result_var + target->idepth_var_smoothed)
    {
        // stats->num_observe_inconsistent++;
        if(plotStereoImages)
            debugImageHypothesisHandling.at<cv::Vec3b>(y, x) = cv::Vec3b(255,255,
                    0);	// Turkoise FOR big inconsistent

        target->idepth_var *= FAIL_VAR_INC_FAC;
        if(target->idepth_var > MAX_VAR) target->isValid = false;

        return false;
    }


    else
    {
        // one more successful observation!
        // stats->num_observe_good++;

        // stats->num_observe_updated++;


        // do textbook ekf update:
        // increase var by a little (prediction-uncertainty)
        float id_var = target->idepth_var*SUCC_VAR_INC_FAC;

        // update var with observation
        float w = result_var / (result_var + id_var);
        float new_idepth = (1-w)*result_idepth + w*target->idepth;
        target->idepth = UNZERO(new_idepth);

        // variance can only decrease from observation; never increase.
        id_var = id_var * w;
        if(id_var < target->idepth_var)
            target->idepth_var = id_var;

        // increase validity!
        target->validity_counter += VALIDITY_COUNTER_INC;
        float absGrad = keyFrameMaxGradBuf[idx];
        if(target->validity_counter > VALIDITY_COUNTER_MAX+absGrad*
                (VALIDITY_COUNTER_MAX_VARIABLE)/255.0f)
            target->validity_counter = VALIDITY_COUNTER_MAX+absGrad*
                                       (VALIDITY_COUNTER_MAX_VARIABLE)/255.0f;

        // increase Skip!
        if(result_eplLength < MIN_EPL_LENGTH_CROP)
        {
            float inc = activeKeyFrame->numFramesTrackedOnThis / (float)(
                            activeKeyFrame->numMappedOnThis+5);
            if(inc < 3) inc = 3;

            inc +=  ((int)(result_eplLength*10000)%2);

            // stats->num_observe_addSkip++;

            if(result_eplLength < 0.5*MIN_EPL_LENGTH_CROP)
                inc *= 3;


            target->nextStereoFrameMinID = refFrame->id() + inc;
        }

        if(plotStereoImages)
            debugImageHypothesisHandling.at<cv::Vec3b>(y, x) = cv::Vec3b(0,255,
                    255); // yellow for GOT UPDATED

        return true;
    }
}

void DepthMap::propagateDepth(Frame* new_keyframe) {
    if(new_keyframe->getTrackingParent() != activeKeyFrame)
    {
        printf("WARNING: propagating depth from frame %d to %d, which was tracked on a different frame (%d).\nWhile this should work, it is not recommended.",
               activeKeyFrame->id(), new_keyframe->id(),
               new_keyframe->getTrackingParent()->id());
    }

    // wipe depthmap
    for(DepthMapPixelHypothesis* pt = otherDepthMap+width*height-1;
            pt >= otherDepthMap; pt--)
    {
        pt->isValid = false;
        pt->blacklisted = 0;
    }

    // re-usable values.
    SE3 oldToNew_SE3 = se3FromSim3(new_keyframe->pose->thisToParent_raw).inverse();
    Eigen::Vector3f trafoInv_t = oldToNew_SE3.translation().cast<float>();
    Eigen::Matrix3f trafoInv_R =
        oldToNew_SE3.rotationMatrix().matrix().cast<float>();


    const bool* trackingWasGood = new_keyframe->getTrackingParent() ==
                                  activeKeyFrame ? new_keyframe->refPixelWasGoodNoCreate() : 0;


    const float* activeKFImageData = activeKeyFrame->image(0);
    const float* newKFMaxGrad = new_keyframe->maxGradients(0);
    const float* newKFImageData = new_keyframe->image(0);





    // go through all pixels of OLD image, propagating forwards.
    for(int y=0; y<height; y++)
        for(int x=0; x<width; x++)
        {
            DepthMapPixelHypothesis* source = currentDepthMap + x + y*width;

            if(!source->isValid)
                continue;

            // runningStats.num_prop_attempts++;


            Eigen::Vector3f pn = (trafoInv_R * Eigen::Vector3f(x*fxi + cxi,y*fyi + cyi,
                                  1.0f)) / source->idepth_smoothed + trafoInv_t;

            float new_idepth = 1.0f / pn[2];

            float u_new = pn[0]*new_idepth*fx + cx;
            float v_new = pn[1]*new_idepth*fy + cy;

            // check if still within image, if not: DROP.
            if(!(u_new > 2.1f && v_new > 2.1f && u_new < width-3.1f
                    && v_new < height-3.1f))
            {
                // runningStats.num_prop_removed_out_of_bounds++;
                continue;
            }

            int newIDX = (int)(u_new+0.5f) + ((int)(v_new+0.5f))*width;
            float destAbsGrad = newKFMaxGrad[newIDX];

            if(trackingWasGood != 0)
            {
                if(!trackingWasGood[(x >> SE3TRACKING_MIN_LEVEL) +
                                    (width >> SE3TRACKING_MIN_LEVEL) *
                                    (y >> SE3TRACKING_MIN_LEVEL)]
                        || destAbsGrad < MIN_ABS_GRAD_DECREASE)
                {
                    // runningStats.num_prop_removed_colorDiff++;
                    continue;
                }
            }
            else
            {
                float sourceColor = activeKFImageData[x + y*width];
                const auto p_((Eigen::Vector2f() << u_new, v_new).finished());
                float destColor = getInterpolatedElement(newKFImageData,
                                                         p_, width);

                float residual = destColor - sourceColor;


                if(residual*residual / (MAX_DIFF_CONSTANT +
                                        MAX_DIFF_GRAD_MULT*destAbsGrad*destAbsGrad) > 1
                        || destAbsGrad < MIN_ABS_GRAD_DECREASE)
                {
                    // runningStats.num_prop_removed_colorDiff++;
                    continue;
                }
            }

            DepthMapPixelHypothesis* targetBest = otherDepthMap +  newIDX;

            // large idepth = point is near = large increase in variance.
            // small idepth = point is far = small increase in variance.
            float idepth_ratio_4 = new_idepth / source->idepth_smoothed;
            idepth_ratio_4 *= idepth_ratio_4;
            idepth_ratio_4 *= idepth_ratio_4;

            float new_var =idepth_ratio_4*source->idepth_var;


            // check for occlusion
            if(targetBest->isValid)
            {
                // if they occlude one another, one gets removed.
                float diff = targetBest->idepth - new_idepth;
                if(DIFF_FAC_PROP_MERGE*diff*diff >
                        new_var +
                        targetBest->idepth_var)
                {
                    if(new_idepth < targetBest->idepth)
                    {
                        // runningStats.num_prop_occluded++;
                        continue;
                    }
                    else
                    {
                        // runningStats.num_prop_occluded++;
                        targetBest->isValid = false;
                    }
                }
            }


            if(!targetBest->isValid)
            {
                // runningStats.num_prop_created++;

                *targetBest = DepthMapPixelHypothesis(
                                  new_idepth,
                                  new_var,
                                  source->validity_counter);

            }
            else
            {
                // runningStats.num_prop_merged++;

                // merge idepth ekf-style
                float w = new_var / (targetBest->idepth_var + new_var);
                float merged_new_idepth = w*targetBest->idepth + (1-w)*new_idepth;

                // merge validity
                int merged_validity = source->validity_counter + targetBest->validity_counter;
                if(merged_validity > VALIDITY_COUNTER_MAX+(VALIDITY_COUNTER_MAX_VARIABLE))
                    merged_validity = VALIDITY_COUNTER_MAX+(VALIDITY_COUNTER_MAX_VARIABLE);

                *targetBest = DepthMapPixelHypothesis(
                                  merged_new_idepth,
                                  1.0f/(1.0f/targetBest->idepth_var + 1.0f/new_var),
                                  merged_validity);
            }
        }

    // swap!
    std::swap(currentDepthMap, otherDepthMap);
}


void DepthMap::regularizeDepthMapFillHolesRow(int yMin, int yMax)
{
    // =========== regularize fill holes
    const float* keyFrameMaxGradBuf = activeKeyFrame->maxGradients(0);

    for(int y=yMin; y<yMax; y++)
    {
        for(int x=3; x<width-2; x++)
        {
            int idx = x+y*width;
            DepthMapPixelHypothesis* dest = otherDepthMap + idx;
            if(dest->isValid) continue;
            if(keyFrameMaxGradBuf[idx]<MIN_ABS_GRAD_DECREASE) continue;

            int* io = validityIntegralBuffer + idx;
            int val = io[2+2*width] - io[2-3*width] - io[-3+2*width] + io[-3-3*width];


            if((dest->blacklisted >= MIN_BLACKLIST && val > VAL_SUM_MIN_FOR_CREATE)
                    || val > VAL_SUM_MIN_FOR_UNBLACKLIST)
            {
                float sumIdepthObs = 0, sumIVarObs = 0;
                int num = 0;

                DepthMapPixelHypothesis* s1max = otherDepthMap + (x-2) + (y+3)*width;
                for (DepthMapPixelHypothesis* s1 = otherDepthMap + (x-2) + (y-2)*width;
                        s1 < s1max; s1+=width)
                    for(DepthMapPixelHypothesis* source = s1; source < s1+5; source++)
                    {
                        if(!source->isValid) continue;

                        sumIdepthObs += source->idepth /source->idepth_var;
                        sumIVarObs += 1.0f/source->idepth_var;
                        num++;
                    }

                float idepthObs = sumIdepthObs / sumIVarObs;
                idepthObs = UNZERO(idepthObs);

                currentDepthMap[idx] =
                    DepthMapPixelHypothesis(
                        idepthObs,
                        VAR_RANDOM_INIT_INITIAL,
                        0);

                // stats->num_reg_created++;
            }
        }
    }
}


void DepthMap::regularizeDepthMapFillHoles()
{

    buildRegIntegralBuffer();

    memcpy(otherDepthMap,currentDepthMap,
           width*height*sizeof(DepthMapPixelHypothesis));
    threadReducer.reduce(boost::bind(&DepthMap::regularizeDepthMapFillHolesRow,
                                     this, _1, _2), 3, height-2, 10);
}



void DepthMap::buildRegIntegralBufferRow1(int yMin, int yMax)
{
    // ============ build inegral buffers
    int* validityIntegralBufferPT = validityIntegralBuffer+yMin*width;
    DepthMapPixelHypothesis* ptSrc = currentDepthMap+yMin*width;
    for(int y=yMin; y<yMax; y++)
    {
        int validityIntegralBufferSUM = 0;

        for(int x=0; x<width; x++)
        {
            if(ptSrc->isValid)
                validityIntegralBufferSUM += ptSrc->validity_counter;

            *(validityIntegralBufferPT++) = validityIntegralBufferSUM;
            ptSrc++;
        }
    }
}


void DepthMap::buildRegIntegralBuffer()
{
    threadReducer.reduce(boost::bind(&DepthMap::buildRegIntegralBufferRow1,
                                     this, _1, _2),
                         0, height);

    int* validityIntegralBufferPT = validityIntegralBuffer;
    int* validityIntegralBufferPT_T = validityIntegralBuffer+width;

    int wh = height*width;
    for(int idx=width; idx<wh; idx++)
        *(validityIntegralBufferPT_T++) += *(validityIntegralBufferPT++);

}



template<bool removeOcclusions> void DepthMap::regularizeDepthMapRow(
    int validityTH, int yMin, int yMax)
{
    const int regularize_radius = 2;

    const float regDistVar = REG_DIST_VAR;

    for(int y=yMin; y<yMax; y++)
    {
        for(int x=regularize_radius; x<width-regularize_radius; x++)
        {
            DepthMapPixelHypothesis* dest = currentDepthMap + x + y*width;
            DepthMapPixelHypothesis* destRead = otherDepthMap + x + y*width;

            // if isValid need to do better examination and then update.

            if(destRead->blacklisted < MIN_BLACKLIST) {
                // stats->num_reg_blacklisted++;
            }

            if(!destRead->isValid)
                continue;

            float sum=0, val_sum=0, sumIvar=0;//, min_varObs = 1e20;
            int numOccluding = 0, numNotOccluding = 0;

            for(int dx=-regularize_radius; dx<=regularize_radius; dx++)
                for(int dy=-regularize_radius; dy<=regularize_radius; dy++)
                {
                    DepthMapPixelHypothesis* source = destRead + dx + dy*width;

                    if(!source->isValid) continue;
//					stats->num_reg_total++;

                    float diff =source->idepth - destRead->idepth;
                    if(DIFF_FAC_SMOOTHING*diff*diff > source->idepth_var + destRead->idepth_var)
                    {
                        if(removeOcclusions)
                        {
                            if(source->idepth > destRead->idepth)
                                numOccluding++;
                        }
                        continue;
                    }

                    val_sum += source->validity_counter;

                    if(removeOcclusions)
                        numNotOccluding++;

                    float distFac = (float)(dx*dx+dy*dy)*regDistVar;
                    float ivar = 1.0f/(source->idepth_var + distFac);

                    sum += source->idepth * ivar;
                    sumIvar += ivar;


                }

            if(val_sum < validityTH)
            {
                dest->isValid = false;
                // stats->num_reg_deleted_secondary++;
                dest->blacklisted--;

                // stats->num_reg_setBlacklisted++;
                continue;
            }


            if(removeOcclusions)
            {
                if(numOccluding > numNotOccluding)
                {
                    dest->isValid = false;
                    // stats->num_reg_deleted_occluded++;

                    continue;
                }
            }

            sum = sum / sumIvar;
            sum = UNZERO(sum);


            // update!
            dest->idepth_smoothed = sum;
            dest->idepth_var_smoothed = 1.0f/sumIvar;

            // stats->num_reg_smeared++;
        }
    }
}
template void DepthMap::regularizeDepthMapRow<true>(int validityTH, int yMin, int yMax);
template void DepthMap::regularizeDepthMapRow<false>(int validityTH, int yMin, int yMax);


void DepthMap::regularizeDepthMap(bool removeOcclusions, int validityTH) {
    memcpy(otherDepthMap,currentDepthMap,
           width*height*sizeof(DepthMapPixelHypothesis));


    if(removeOcclusions)
        threadReducer.reduce(boost::bind(&DepthMap::regularizeDepthMapRow<true>,
                                         this, validityTH, _1, _2),
                             2, height-2, 10);
    else
        threadReducer.reduce(boost::bind(&DepthMap::regularizeDepthMapRow<false>,
                                         this, validityTH, _1, _2),
                             2, height-2, 10);
}


void DepthMap::initializeRandomly(Frame* new_frame)
{
    activeKeyFramelock = new_frame->getActiveLock();
    activeKeyFrame = new_frame;
    activeKeyFrameImageData = activeKeyFrame->image(0);
    activeKeyFrameIsReactivated = false;

    const float* maxGradients = new_frame->maxGradients();

    for(int y=1; y<height-1; y++)
    {
        for(int x=1; x<width-1; x++)
        {
            if(maxGradients[x+y*width] > MIN_ABS_GRAD_CREATE)
            {
                float idepth = 0.5f + 1.0f * ((rand() % 100001) / 100000.0f);
                currentDepthMap[x+y*width] = DepthMapPixelHypothesis(
                                                 idepth,
                                                 idepth,
                                                 VAR_RANDOM_INIT_INITIAL,
                                                 VAR_RANDOM_INIT_INITIAL,
                                                 20);
            }
            else
            {
                currentDepthMap[x+y*width].isValid = false;
                currentDepthMap[x+y*width].blacklisted = 0;
            }
        }
    }


    activeKeyFrame->setDepth(currentDepthMap);
}



void DepthMap::setFromExistingKF(Frame* kf)
{
    assert(kf->hasIDepthBeenSet());

    activeKeyFramelock = kf->getActiveLock();
    activeKeyFrame = kf;

    const float* idepth = activeKeyFrame->idepth_reAct();
    const float* idepthVar = activeKeyFrame->idepthVar_reAct();
    const unsigned char* validity = activeKeyFrame->validity_reAct();

    DepthMapPixelHypothesis* pt = currentDepthMap;
    activeKeyFrame->numMappedOnThis = 0;
    activeKeyFrame->numFramesTrackedOnThis = 0;
    activeKeyFrameImageData = activeKeyFrame->image(0);
    activeKeyFrameIsReactivated = true;

    for(int y=0; y<height; y++)
    {
        for(int x=0; x<width; x++)
        {
            if(*idepthVar > 0)
            {
                *pt = DepthMapPixelHypothesis(
                          *idepth,
                          *idepthVar,
                          *validity);
            }
            else
            {
                currentDepthMap[x+y*width].isValid = false;
                currentDepthMap[x+y*width].blacklisted = (*idepthVar == -2) ? MIN_BLACKLIST-1 :
                        0;
            }

            idepth++;
            idepthVar++;
            validity++;
            pt++;
        }
    }

    regularizeDepthMap(false, VAL_SUM_MIN_FOR_KEEP);
}


void DepthMap::initializeFromGTDepth(Frame* new_frame)
{
    assert(new_frame->hasIDepthBeenSet());

    activeKeyFramelock = new_frame->getActiveLock();
    activeKeyFrame = new_frame;
    activeKeyFrameImageData = activeKeyFrame->image(0);
    activeKeyFrameIsReactivated = false;

    const float* idepth = new_frame->idepth();


    float averageGTIDepthSum = 0;
    int averageGTIDepthNum = 0;
    for(int y=0; y<height; y++)
    {
        for(int x=0; x<width; x++)
        {
            float idepthValue = idepth[x+y*width];
            if(!std::isnan(idepthValue) && idepthValue > 0)
            {
                averageGTIDepthSum += idepthValue;
                averageGTIDepthNum ++;
            }
        }
    }


    for(int y=0; y<height; y++)
    {
        for(int x=0; x<width; x++)
        {
            float idepthValue = idepth[x+y*width];

            if(!std::isnan(idepthValue) && idepthValue > 0)
            {
                currentDepthMap[x+y*width] = DepthMapPixelHypothesis(
                                                 idepthValue,
                                                 idepthValue,
                                                 VAR_GT_INIT_INITIAL,
                                                 VAR_GT_INIT_INITIAL,
                                                 20);
            }
            else
            {
                currentDepthMap[x+y*width].isValid = false;
                currentDepthMap[x+y*width].blacklisted = 0;
            }
        }
    }


    activeKeyFrame->setDepth(currentDepthMap);
}


void DepthMap::updateKeyframe(std::deque< std::shared_ptr<Frame> >
                              referenceFrames)
{
    assert(isValid());

    timepoint_t tv_start_all, tv_end_all;
    // gettimeofday(&tv_start_all, NULL);
    tv_start_all  = std::chrono::high_resolution_clock::now();

    oldest_referenceFrame = referenceFrames.front().get();
    newest_referenceFrame = referenceFrames.back().get();
    referenceFrameByID.clear();
    referenceFrameByID_offset = oldest_referenceFrame->id();

    for(std::shared_ptr<Frame> frame : referenceFrames)
    {
        assert(frame->hasTrackingParent());

        if(frame->getTrackingParent() != activeKeyFrame)
        {
            printf("WARNING: updating frame %d with %d, which was tracked on a different frame (%d).\nWhile this should work, it is not recommended.",
                   activeKeyFrame->id(), frame->id(),
                   frame->getTrackingParent()->id());
        }

        Sim3 refToKf;
        if(frame->pose->trackingParent->frameID == activeKeyFrame->id())
            refToKf = frame->pose->thisToParent_raw;
        else
            refToKf = activeKeyFrame->getScaledCamToWorld().inverse() *
                      frame->getScaledCamToWorld();

        frame->prepareForStereoWith(activeKeyFrame, refToKf, K, 0);

        while((int)referenceFrameByID.size() + referenceFrameByID_offset <=
                frame->id())
            referenceFrameByID.push_back(frame.get());
    }


    if(plotStereoImages)
    {
        cv::Mat keyFrameImage(activeKeyFrame->height(), activeKeyFrame->width(),
                              CV_32F, const_cast<float*>(activeKeyFrameImageData));
        keyFrameImage.convertTo(debugImageHypothesisHandling, CV_8UC1);
        cv::cvtColor(debugImageHypothesisHandling, debugImageHypothesisHandling,
                     CV_GRAY2RGB);

        cv::Mat oldest_refImage(oldest_referenceFrame->height(),
                                oldest_referenceFrame->width(), CV_32F,
                                const_cast<float*>(oldest_referenceFrame->image(0)));
        cv::Mat newest_refImage(newest_referenceFrame->height(),
                                newest_referenceFrame->width(), CV_32F,
                                const_cast<float*>(newest_referenceFrame->image(0)));
        cv::Mat rfimg = 0.5f*oldest_refImage + 0.5f*newest_refImage;
        rfimg.convertTo(debugImageStereoLines, CV_8UC1);
        cv::cvtColor(debugImageStereoLines, debugImageStereoLines, CV_GRAY2RGB);
    }

    timepoint_t tv_start, tv_end;


    // gettimeofday(&tv_start, NULL);
    tv_start = std::chrono::high_resolution_clock::now();
    observeDepth();
    // gettimeofday(&tv_end, NULL);
    tv_end = std::chrono::high_resolution_clock::now();
    msObserve = 0.9*msObserve +
                0.1*std::chrono::duration_cast<std::chrono::milliseconds>
                (tv_end - tv_start).count();
    nObserve++;

    //if(rand()%10==0)
    {
        // gettimeofday(&tv_start, NULL);
        tv_start = std::chrono::high_resolution_clock::now();
        regularizeDepthMapFillHoles();
        // gettimeofday(&tv_end, NULL);
        tv_end = std::chrono::high_resolution_clock::now();
        msFillHoles = 0.9*msFillHoles +
                      0.1*std::chrono::duration_cast<std::chrono::milliseconds>
                      (tv_end - tv_start).count();
        nFillHoles++;
    }


    // gettimeofday(&tv_start, NULL);
    tv_start = std::chrono::high_resolution_clock::now();
    regularizeDepthMap(false, VAL_SUM_MIN_FOR_KEEP);
    // gettimeofday(&tv_end, NULL);
    tv_end = std::chrono::high_resolution_clock::now();
    msRegularize = 0.9*msRegularize +
                   0.1*std::chrono::duration_cast<std::chrono::milliseconds>
                   (tv_end - tv_start).count();
    nRegularize++;


    // Update depth in keyframe
    if(!activeKeyFrame->depthHasBeenUpdatedFlag)
    {
        // gettimeofday(&tv_start, NULL);
        tv_start = std::chrono::high_resolution_clock::now();
        activeKeyFrame->setDepth(currentDepthMap);
        // gettimeofday(&tv_end, NULL);
        tv_end = std::chrono::high_resolution_clock::now();
        msSetDepth = 0.9*msSetDepth +
                     0.1*std::chrono::duration_cast<std::chrono::milliseconds>
                     (tv_end - tv_start).count();
        nSetDepth++;
    }


    // gettimeofday(&tv_end_all, NULL);
    tv_end_all = std::chrono::high_resolution_clock::now();
    msUpdate = 0.9*msUpdate +
               0.1*std::chrono::duration_cast<std::chrono::milliseconds>
               (tv_end_all - tv_start_all).count();
    nUpdate++;


    activeKeyFrame->numMappedOnThis++;
    activeKeyFrame->numMappedOnThisTotal++;


    if(plotStereoImages)
    {
        Util::displayImage( "Stereo Key Frame", debugImageHypothesisHandling, false );
        Util::displayImage( "Stereo Reference Frame", debugImageStereoLines, false );
    }
}

void DepthMap::invalidate()
{
    if(activeKeyFrame==0) return;
    activeKeyFrame=0;
    activeKeyFramelock.unlock();
}

void DepthMap::createKeyFrame(Frame* new_keyframe)
{
    assert(isValid());
    assert(new_keyframe != nullptr);
    assert(new_keyframe->hasTrackingParent());

    //boost::shared_lock<boost::shared_mutex> lock = activeKeyFrame->getActiveLock();
    boost::shared_lock<boost::shared_mutex> lock2 = new_keyframe->getActiveLock();

    timepoint_t tv_start_all, tv_end_all;
    // gettimeofday(&tv_start_all, NULL);
    tv_start_all = std::chrono::high_resolution_clock::now();


    if(plotStereoImages)
    {
        cv::Mat keyFrameImage(new_keyframe->height(), new_keyframe->width(), CV_32F,
                              const_cast<float*>(new_keyframe->image(0)));
        keyFrameImage.convertTo(debugImageHypothesisPropagation, CV_8UC1);
        cv::cvtColor(debugImageHypothesisPropagation, debugImageHypothesisPropagation,
                     CV_GRAY2RGB);
    }



    SE3 oldToNew_SE3 = se3FromSim3(new_keyframe->pose->thisToParent_raw).inverse();

    timepoint_t tv_start, tv_end;
    // gettimeofday(&tv_start, NULL);
    tv_start = std::chrono::high_resolution_clock::now();
    propagateDepth(new_keyframe);
    // gettimeofday(&tv_end, NULL);
    tv_end = std::chrono::high_resolution_clock::now();
    msPropagate = 0.9*msPropagate +
                  0.1*std::chrono::duration_cast<std::chrono::milliseconds>
                  (tv_end - tv_start).count();
    nPropagate++;

    activeKeyFrame = new_keyframe;
    activeKeyFramelock = activeKeyFrame->getActiveLock();
    activeKeyFrameImageData = new_keyframe->image(0);
    activeKeyFrameIsReactivated = false;



    // gettimeofday(&tv_start, NULL);
    tv_start = std::chrono::high_resolution_clock::now();
    regularizeDepthMap(true, VAL_SUM_MIN_FOR_KEEP);
    // gettimeofday(&tv_end, NULL);
    tv_end = std::chrono::high_resolution_clock::now();
    msRegularize = 0.9*msRegularize +
                   0.1*std::chrono::duration_cast<std::chrono::milliseconds>
                   (tv_end - tv_start).count();
    nRegularize++;


    // gettimeofday(&tv_start, NULL);
    tv_start = std::chrono::high_resolution_clock::now();
    regularizeDepthMapFillHoles();
    // gettimeofday(&tv_end, NULL);
    tv_end = std::chrono::high_resolution_clock::now();
    msFillHoles = 0.9*msFillHoles +
                  0.1*std::chrono::duration_cast<std::chrono::milliseconds>
                  (tv_end - tv_start).count();
    nFillHoles++;


    // gettimeofday(&tv_start, NULL);
    tv_start = std::chrono::high_resolution_clock::now();
    regularizeDepthMap(false, VAL_SUM_MIN_FOR_KEEP);
    // gettimeofday(&tv_end, NULL);
    tv_end = std::chrono::high_resolution_clock::now();
    msRegularize = 0.9*msRegularize +
                   0.1*std::chrono::duration_cast<std::chrono::milliseconds>
                   (tv_end - tv_start).count();
    nRegularize++;




    // make mean inverse depth be one.
    float sumIdepth=0, numIdepth=0;
    for(DepthMapPixelHypothesis* source = currentDepthMap;
            source < currentDepthMap+width*height; source++)
    {
        if(!source->isValid)
            continue;
        sumIdepth += source->idepth_smoothed;
        numIdepth++;
    }
    float rescaleFactor = numIdepth / sumIdepth;
    float rescaleFactor2 = rescaleFactor*rescaleFactor;
    for(DepthMapPixelHypothesis* source = currentDepthMap;
            source < currentDepthMap+width*height; source++)
    {
        if(!source->isValid)
            continue;
        source->idepth *= rescaleFactor;
        source->idepth_smoothed *= rescaleFactor;
        source->idepth_var *= rescaleFactor2;
        source->idepth_var_smoothed *= rescaleFactor2;
    }
    activeKeyFrame->pose->thisToParent_raw = sim3FromSE3(oldToNew_SE3.inverse(),
            rescaleFactor);
    activeKeyFrame->pose->invalidateCache();

    // Update depth in keyframe

    // gettimeofday(&tv_start, NULL);
    tv_start = std::chrono::high_resolution_clock::now();
    activeKeyFrame->setDepth(currentDepthMap);
    // gettimeofday(&tv_end, NULL);
    tv_end = std::chrono::high_resolution_clock::now();
    msSetDepth = 0.9*msSetDepth +
                 0.1*std::chrono::duration_cast<std::chrono::milliseconds>
                 (tv_end - tv_start).count();
    nSetDepth++;

    //gettimeofday(&tv_end_all, NULL);
    tv_end_all = std::chrono::high_resolution_clock::now();
    msCreate = 0.9*msCreate +
               0.1*std::chrono::duration_cast<std::chrono::milliseconds>
               (tv_end_all - tv_start_all).count();
    nCreate++;



    if(plotStereoImages)
    {
        //Util::displayImage( "KeyFramePropagation", debugImageHypothesisPropagation );
    }

}

void DepthMap::addTimingSample()
{
    timepoint_t now;
    // gettimeofday(&now, NULL);
    now = std::chrono::high_resolution_clock::now();
    // float sPassed = ((now.tv_sec-lastHzUpdate.tv_sec) + (now.tv_usec-lastHzUpdate.tv_usec)/1000000.0f);
    float sPassed = std::chrono::duration_cast<std::chrono::seconds>
                    (now - lastHzUpdate).count();
    if(sPassed > 1.0f)
    {
        nAvgUpdate = 0.8*nAvgUpdate + 0.2*(nUpdate / sPassed);
        nUpdate = 0;
        nAvgCreate = 0.8*nAvgCreate + 0.2*(nCreate / sPassed);
        nCreate = 0;
        nAvgFinalize = 0.8*nAvgFinalize + 0.2*(nFinalize / sPassed);
        nFinalize = 0;
        nAvgObserve = 0.8*nAvgObserve + 0.2*(nObserve / sPassed);
        nObserve = 0;
        nAvgRegularize = 0.8*nAvgRegularize + 0.2*(nRegularize / sPassed);
        nRegularize = 0;
        nAvgPropagate = 0.8*nAvgPropagate + 0.2*(nPropagate / sPassed);
        nPropagate = 0;
        nAvgFillHoles = 0.8*nAvgFillHoles + 0.2*(nFillHoles / sPassed);
        nFillHoles = 0;
        nAvgSetDepth = 0.8*nAvgSetDepth + 0.2*(nSetDepth / sPassed);
        nSetDepth = 0;
        lastHzUpdate = now;

        if(enablePrintDebugInfo && printMappingTiming)
        {
            printf("Upd %3.1fms (%.1fHz); Create %3.1fms (%.1fHz); Final %3.1fms (%.1fHz) // Obs %3.1fms (%.1fHz); Reg %3.1fms (%.1fHz); Prop %3.1fms (%.1fHz); Fill %3.1fms (%.1fHz); Set %3.1fms (%.1fHz)\n",
                   msUpdate, nAvgUpdate,
                   msCreate, nAvgCreate,
                   msFinalize, nAvgFinalize,
                   msObserve, nAvgObserve,
                   msRegularize, nAvgRegularize,
                   msPropagate, nAvgPropagate,
                   msFillHoles, nAvgFillHoles,
                   msSetDepth, nAvgSetDepth);
        }
    }


}

void DepthMap::finalizeKeyFrame()
{
    assert(isValid());


    timepoint_t tv_start_all, tv_end_all;
    //gettimeofday(&tv_start_all, NULL);
    tv_start_all = std::chrono::high_resolution_clock::now();
    timepoint_t tv_start, tv_end;

    // gettimeofday(&tv_start, NULL);
    tv_start = std::chrono::high_resolution_clock::now();
    regularizeDepthMapFillHoles();
    // gettimeofday(&tv_end, NULL);
    tv_end = std::chrono::high_resolution_clock::now();
    msFillHoles = 0.9*msFillHoles +
                  0.1*std::chrono::duration_cast<std::chrono::milliseconds>
                  (tv_end - tv_start).count();
    nFillHoles++;

    // gettimeofday(&tv_start, NULL);
    tv_start = std::chrono::high_resolution_clock::now();
    regularizeDepthMap(false, VAL_SUM_MIN_FOR_KEEP);
    // gettimeofday(&tv_end, NULL);
    tv_end = std::chrono::high_resolution_clock::now();
    msRegularize = 0.9*msRegularize +
                   0.1*std::chrono::duration_cast<std::chrono::milliseconds>
                   (tv_end - tv_start).count();
    nRegularize++;

    // gettimeofday(&tv_start, NULL);
    tv_start = std::chrono::high_resolution_clock::now();
    activeKeyFrame->setDepth(currentDepthMap);
    activeKeyFrame->calculateMeanInformation();
    activeKeyFrame->takeReActivationData(currentDepthMap);
    // gettimeofday(&tv_end, NULL);
    tv_end = std::chrono::high_resolution_clock::now();
    msSetDepth = 0.9*msSetDepth +
                 0.1*std::chrono::duration_cast<std::chrono::milliseconds>
                 (tv_end - tv_start).count();
    nSetDepth++;

    //gettimeofday(&tv_end_all, NULL);
    tv_end_all = std::chrono::high_resolution_clock::now();
    msFinalize = 0.9*msFinalize +
                 0.1*std::chrono::duration_cast<std::chrono::milliseconds>
                 (tv_end_all - tv_start_all).count();
    nFinalize++;
}




int DepthMap::debugPlotDepthMap()
{
    if(activeKeyFrame == 0) return 1;

    cv::Mat keyFrameImage(activeKeyFrame->height(), activeKeyFrame->width(),
                          CV_32F, const_cast<float*>(activeKeyFrameImageData));
    keyFrameImage.convertTo(debugImageDepth, CV_8UC1);
    cv::cvtColor(debugImageDepth, debugImageDepth, CV_GRAY2RGB);

    // debug plot & publish sparse version?
    int refID = referenceFrameByID_offset;


    for(int y=0; y<height; y++)
        for(int x=0; x<width; x++)
        {
            int idx = x + y*width;

            if(currentDepthMap[idx].blacklisted < MIN_BLACKLIST && debugDisplay == 2)
                debugImageDepth.at<cv::Vec3b>(y,x) = cv::Vec3b(0,0,255);

            if(!currentDepthMap[idx].isValid) continue;

            cv::Vec3b color = currentDepthMap[idx].getVisualizationColor(refID);
            debugImageDepth.at<cv::Vec3b>(y,x) = color;
        }


    return 1;
}


bool search_range_is_in_image_area(const Eigen::Vector2f &start,
                                   const Eigen::Vector2f &end,
                                   const Eigen::Vector2i &image_size) {
    // 2 comes from the one-sided gradient calculation at the bottom
    return is_in_image_range(start, image_size, 2) &&
           is_in_image_range(end, image_size, 2);
}


Eigen::VectorXf intensities_along_line(const float *image, const int image_width,
                                       const Eigen::Vector2f &center_coordinate,
                                       const Eigen::Vector2f &step) {
    // calculate values to search for
    Eigen::VectorXf intensities(5);
    intensities[0] = getInterpolatedElement(image, center_coordinate - 2 * step, image_width);
    intensities[1] = getInterpolatedElement(image, center_coordinate - 1 * step, image_width);
    intensities[2] = getInterpolatedElement(image, center_coordinate - 0 * step, image_width);
    intensities[3] = getInterpolatedElement(image, center_coordinate + 1 * step, image_width);
    intensities[4] = getInterpolatedElement(image, center_coordinate + 2 * step, image_width);
    return intensities;
}

// find pixel in image (do stereo along epipolar line).
// mat: NEW image
// KinvP: point in OLD image (Kinv * (u_old, v_old, 1)), projected
// trafo: x_old = trafo * x_new; (from new to old image)
// key_intensities[2]: descriptor in OLD image.
// returns: result_idepth : point depth in new camera's coordinate system
// returns: result_u/v : point's coordinates in new camera's coordinate system
// returns: idepth_var: (approximated) measurement variance of inverse depth of result_point_NEW
// returns error if sucessful; -1 if out of bounds, -2 if not found.
inline float DepthMap::doLineStereo(
    const Eigen::Vector2i &keyframe_coordinate_,
    const float min_idepth_along_t, const float prior_idepth_key,
    float max_idepth_along_t,
    const Frame* const referenceFrame, const float* referenceFrameImage,
    float &result_idepth, float &result_var, float &result_eplLength) {
    const Eigen::Vector2i image_size(width, height);

    Eigen::Vector2f key_epipolar_direction;
    bool isGood = makeAndCheckEPL(keyframe_coordinate_,
                                  referenceFrame->thisToOther_t,
                                  key_epipolar_direction);
    if(!isGood) return -5;

    // stats->num_stereo_calls++;

    const Eigen::Vector2f keyframe_coordinate = keyframe_coordinate_.cast<float>();

    // calculate epipolar line start and end point in old image
    const Eigen::Vector3f KinvP = KInv * tohomogeneous(keyframe_coordinate);
    const Eigen::Vector3f P_key = KinvP / prior_idepth_key;
    // P_key seen from the reference frame
    const Eigen::Vector3f P_ref = referenceFrame->K_otherToThis_R * P_key
                                + referenceFrame->K_otherToThis_t;

    const float idepth_ref = 1 / P_ref[2];
    // depth ratio of P_ref per P_key
    const float inv_depth_ratio = prior_idepth_key / idepth_ref;

    const float key_sample_distance =
        REFERENCE_SAMPLE_DISTANCE * inv_depth_ratio;

    if (not search_range_is_in_image_area(
            keyframe_coordinate - 2*key_epipolar_direction*key_sample_distance,
            keyframe_coordinate + 2*key_epipolar_direction*key_sample_distance,
            image_size)) {
        return -1;
    }

    if(!(inv_depth_ratio > 0.7f && inv_depth_ratio < 1.4f)) {
        // stats->num_stereo_rescale_oob++;
        return -1;
    }

    Eigen::Vector3f _pClose = referenceFrame->K_otherToThis_R * KinvP
                            + referenceFrame->K_otherToThis_t * max_idepth_along_t;
    // if the assumed close-point lies behind the
    // image, have to change that.
    if(_pClose[2] < 0.001) {
        const Eigen::Vector3f pInf = referenceFrame->K_otherToThis_R * KinvP;
        max_idepth_along_t = (0.001-pInf[2]) / referenceFrame->K_otherToThis_t[2];
        _pClose = pInf + referenceFrame->K_otherToThis_t*max_idepth_along_t;
    }
    // pos in new image of point (xy), assuming max_idepth_along_t
    Eigen::Vector2f pClose = projection(_pClose);

    Eigen::Vector3f _pFar = referenceFrame->K_otherToThis_R * KinvP
                          + referenceFrame->K_otherToThis_t * min_idepth_along_t;
    // if the assumed far-point lies behind the image or closter than the near-point,
    // we moved past the Point it and should stop.
    if(_pFar[2] < 0.001 || max_idepth_along_t < min_idepth_along_t) {
        // stats->num_stereo_inf_oob++;
        return -1;
    }
    // pos in new image of point (xy), assuming min_idepth_along_t
    Eigen::Vector2f pFar = projection(_pFar);

    // check for nan due to eg division by zero.
    // if(std::isnan((float)(pFar[0] + pClose[0])))
    //     return -4;

    // calculate increments in which we will step through the epipolar line.
    // they are key_sample_distance (or half sample dist) long
    const Eigen::Vector2f ref_search_step =
        REFERENCE_SAMPLE_DISTANCE * normalize_length(pClose - pFar);
    float eplLength = (pClose - pFar).norm();
    if(!eplLength > 0 || std::isinf(eplLength)) return -4;

    if(eplLength > MAX_EPL_LENGTH_CROP) {
        pClose = pFar + normalize_length(pClose - pFar) * MAX_EPL_LENGTH_CROP;
    }

    // extend one sample_dist to left & right.
    pFar -= ref_search_step;
    pClose += ref_search_step;

    // make epl long enough (pad a little bit).
    if(eplLength < MIN_EPL_LENGTH_CROP) {
        float pad = (MIN_EPL_LENGTH_CROP - (eplLength)) / 2;
        pFar -= ref_search_step*pad;
        pClose += ref_search_step*pad;
    }

    if((not is_in_image_range(pFar, image_size, SAMPLE_POINT_TO_BORDER+1)) ||
       (not is_in_image_range(pClose, image_size, 1))) {
        // stats->num_stereo_inf_oob++;
        // stats->num_stereo_near_oob++;
        return -1;
    }

    // from here on:
    // - pInf: search start-point
    // - p0: search end-point
    // - ref_search_step: search step in pixel
    // - eplLength, min_idepth_along_t, max_idepth_along_t:
    // determines search-resolution, i.e. the result's variance.

    /*
     * Subsequent exact minimum is found the following way:
     * - assuming lin. interpolation, the gradient of Error at p1 (towards p2) is given by
     *   dE1 = -2sum(e1*e1 - e1*e2)
     *   where e1 and e2 are summed over, and are the residuals (not squared).
     *
     * - the gradient at p2 (coming from p1) is given by
     * 	 dE2 = +2sum(e2*e2 - e1*e2)
     *
     * - linear interpolation => gradient changes linearely; zero-crossing is hence given by
     *   p1 + d*(p2-p1) with d = -dE1 / (-dE1 + dE2).
     *
     *
     *
     * => I for later exact min calculation, I need sum(e_i*e_i),sum(e_{i-1}*e_{i-1}),sum(e_{i+1}*e_{i+1})
     *    and sum(e_i * e_{i-1}) and sum(e_i * e_{i+1}),
     *    where i is the respective winning index.
     */

    const Eigen::VectorXf &key_intensities = intensities_along_line(
        activeKeyFrameImageData, width,
        keyframe_coordinate, key_epipolar_direction * key_sample_distance
    );

    Eigen::VectorXf ref_intensities(5);
    ref_intensities[0] = getInterpolatedElement(referenceFrameImage,
                                                pFar - 2 * ref_search_step, width);
    ref_intensities[1] = getInterpolatedElement(referenceFrameImage,
                                                pFar - 1 * ref_search_step, width);
    ref_intensities[2] = getInterpolatedElement(referenceFrameImage,
                                                pFar - 0 * ref_search_step, width);
    ref_intensities[3] = getInterpolatedElement(referenceFrameImage,
                                                pFar + 1 * ref_search_step, width);

    Eigen::Vector2f search_point_ref = pFar;

    // walk in equally sized steps, starting at depth=infinity.
    Eigen::Vector2f argmin_point_ref(-1, -1);
    float min_error = 1e50;
    float second_min_error = 1e50;

    // best pre and post errors.
    float prev_error=NAN,
          next_error=NAN,
          prev_diff=NAN,
          next_diff=NAN;

    float prev_error_ = -1; // final error of last comp.

    // alternating intermediate vars
    Eigen::VectorXf eA(5), eB(5);

    int curr_argmin=-1, second_argmin =-1;

    for (int i = 0; ; i++) {
        if((ref_search_step[0] < 0) != (search_point_ref[0] > pClose[0]) ||
           (ref_search_step[1] < 0) != (search_point_ref[1] > pClose[1])) {
            break;
        }

        // interpolate one new point
        ref_intensities[4] = getInterpolatedElement(
            referenceFrameImage, search_point_ref + 2 * ref_search_step, width);

        // hacky but fast way to get error and differential error:
        // switch buffer variables for last loop.
        // calc error and accumulate sums.
        if(i % 2 == 0) {
            eA = ref_intensities - key_intensities;
        } else {
            eB = ref_intensities - key_intensities;
        }

        float error = (ref_intensities - key_intensities).squaredNorm();
        // do I have a new winner??
        // if so: set.
        if(error < min_error) {
            // put to second-best
            second_min_error = min_error;
            second_argmin = curr_argmin;

            // set best.
            min_error = error;
            curr_argmin = i;

            prev_error = prev_error_;
            prev_diff = eA.dot(eB);
            next_error = -1;
            next_diff = -1;

            argmin_point_ref = search_point_ref;
        } else {
        // otherwise: the last might be the current winner,
        // in which case i have to save these values.
            if(i - 1 == curr_argmin) {
                next_error = error;
                next_diff = eA.dot(eB);
            }

            // collect second-best:
            // just take the best of all that are NOT equal to current best.
            if(error < second_min_error) {
                second_min_error = error;
                second_argmin = i;
            }
        }

        // shift everything one further.
        prev_error_ = error;
        ref_intensities.head(4) = ref_intensities.tail(4);

        // stats->num_stereo_comparisons++;

        search_point_ref += ref_search_step;
    }

    // if error too big, will return -3, otherwise -2.
    if(min_error > 4*(float)MAX_ERROR_STEREO) {
        // stats->num_stereo_invalid_bigErr++;
        return -3;
    }

    // check if clear enough winner
    if(abs(curr_argmin - second_argmin) > 1 &&
       MIN_DISTANCE_ERROR_STEREO * min_error > second_min_error) {
        // stats->num_stereo_invalid_unclear_winner++;
        return -2;
    }

    // final decisions here.
    bool interpolate_next = false;
    bool interpolate_prev = false;
    if(useSubpixelStereo) {
        // ================== compute exact match =========================
        // compute gradients (they are actually only half the real gradient)
        float grad_prev_prev = -(prev_error - prev_diff);
        float grad_prev_curr = +(min_error - prev_diff);
        float grad_next_curr = -(min_error - next_diff);
        float grad_next_next = +(next_error - next_diff);

        // if one is oob: return false.
        if(enablePrintDebugInfo && (prev_error < 0 || next_error < 0)) {
            // stats->num_stereo_invalid_atEnd++;
        } else if((grad_next_curr < 0) ^ (grad_prev_curr < 0)) {
            // - if zero-crossing occurs exactly in between (gradient Inconsistent),
            // return exact pos, if both central gradients are small
            // compared to their counterpart.
            if(enablePrintDebugInfo &&
              (grad_next_curr*grad_next_curr > 0.1f*0.1f*grad_next_next*grad_next_next ||
               grad_prev_curr*grad_prev_curr > 0.1f*0.1f*grad_prev_prev*grad_prev_prev)) {
                // stats->num_stereo_invalid_inexistantCrossing++;
            }
        } else if((grad_prev_prev < 0) ^ (grad_prev_curr < 0)) {
        // if pre has zero-crossing
            // if post has zero-crossing
            if((grad_next_next < 0) ^ (grad_next_curr < 0)) {
                // stats->num_stereo_invalid_twoCrossing++;
            } else {
                interpolate_prev = true;
            }
        } else if((grad_next_next < 0) ^ (grad_next_curr < 0)) {
            // if post has zero-crossing
            interpolate_next = true;
        } else {
            // if none has zero-crossing
            // stats->num_stereo_invalid_noCrossing++;
        }

        // DO interpolation!
        // minimum occurs at zero-crossing of gradient, which is a straight line => easy to compute.
        // the error at that point is also computed by just integrating.
        if(interpolate_prev) {
            float d = grad_prev_curr / (grad_prev_curr - grad_prev_prev);
            argmin_point_ref -= d*ref_search_step;
            min_error = min_error - 2*d*grad_prev_curr
                       - (grad_prev_prev - grad_prev_curr)*d*d;
            // stats->num_stereo_interpPre++;
        } else if(interpolate_next) {
            float d = grad_next_curr / (grad_next_curr - grad_next_next);
            argmin_point_ref += d*ref_search_step;
            min_error = min_error + 2*d*grad_next_curr
                       + (grad_next_next - grad_next_curr)*d*d;
            // stats->num_stereo_interpPost++;
        } else {
            // stats->num_stereo_interpNone++;
        }
    }


    const float gradAlongLine = calc_grad_along_line(key_intensities, key_sample_distance);

    // check if interpolated error is OK. use evil hack to allow more error if there is a lot of gradient.
    if(min_error > (float)MAX_ERROR_STEREO + sqrtf(gradAlongLine)*20) {
        // stats->num_stereo_invalid_bigErr++;
        return -3;
    }

    // ================= calc depth (in KF) ====================
    // * KinvP = Kinv * (x,y,1); where x,y are pixel coordinates of point we search for,
    // in the KF.
    // * argmin_point_ref[0] = x-coordinate of found correspondence in the reference frame.

    float idnew_best_match;	// depth in the new image
    // d(idnew_best_match) / d(disparity in pixel) == conputed
    // inverse depth derived by the pixel-disparity.
    const Eigen::Vector3f RKinvP = referenceFrame->otherToThis_R * KinvP;
    float alpha;

    const Eigen::Vector3f inv_cp = KInv * tohomogeneous(argmin_point_ref);
    const Eigen::Vector3f &key_to_ref_t = referenceFrame->otherToThis_t;
    const Eigen::Vector2f beta =
        RKinvP.head(2) * key_to_ref_t[2] - RKinvP[2] * key_to_ref_t.head(2);
    const Eigen::Vector2f nominators =
        inv_cp.head(2) * key_to_ref_t[2] - inv_cp[2] * key_to_ref_t.head(2);
    const Eigen::Vector2f inv_focal_lengths(fxi, fyi);
    const Eigen::Vector2f alphas =
        ref_search_step.array() * inv_focal_lengths.array() * beta.array() /
        (nominators.array() * nominators.array());

    const Eigen::Vector2f idnew_best_matches =
        (RKinvP.head(2) * inv_cp[2] - RKinvP[2] * inv_cp.head(2)).array() /
        nominators.array();

    if(ref_search_step[0]*ref_search_step[0]>ref_search_step[1]*ref_search_step[1]) {
        idnew_best_match = idnew_best_matches[0];
        alpha = alphas[0];
    } else {
        idnew_best_match = idnew_best_matches[1];
        alpha = alphas[1];
    }

    if(idnew_best_match < 0) {
        // stats->num_stereo_negative++;
        if(!allowNegativeIdepths)
            return -2;
    }

    // stats->num_stereo_successfull++;

    // ================= calc var (in NEW image) ====================

    const float geoDispError = calc_geometric_disparity_error(
        getInterpolatedElement42(activeKeyFrame->gradients(0),
                                 keyframe_coordinate, width),
        key_epipolar_direction * REFERENCE_SAMPLE_DISTANCE,
        referenceFrame->initialTrackedResidual
    );

    // geoDispError *= (0.5 + 0.5 *result_idepth) * (0.5 + 0.5 *result_idepth);

    // final error consists of a small constant part (discretization error),
    // geometric and photometric error.
    float coeff = (interpolate_prev || interpolate_next) ? 0.05f : 0.5f;
    const float photoDispError = 4 * cameraPixelNoise2 / (gradAlongLine + DIVISION_EPS);
    // square to make variance
    result_var = alpha*alpha*(
        coeff*key_sample_distance*key_sample_distance + geoDispError + photoDispError);

    if(plotStereoImages)
    {
        if(rand()%5==0)
        {
            //if(rand()%500 == 0)
            //	printf("geo: %f, photo: %f, alpha: %f\n", sqrt(geoDispError), sqrt(photoDispError), alpha, sqrt(result_var));


            //int idDiff = (keyFrame->pyramidID - referenceFrame->id);
            //cv::Scalar color = cv::Scalar(0,0, 2*idDiff);// bw

            //cv::Scalar color = cv::Scalar(sqrt(result_var)*2000, 255-sqrt(result_var)*2000, 0);// bw

//			float eplLengthF = std::min((float)MIN_EPL_LENGTH_CROP,(float)eplLength);
//			eplLengthF = std::max((float)MAX_EPL_LENGTH_CROP,(float)eplLengthF);
//
//			float pixelDistFound = sqrtf((float)((P_ref[0]/P_ref[2] - argmin_point_ref[0])*(P_ref[0]/P_ref[2] - argmin_point_ref[0])
//					+ (P_ref[1]/P_ref[2] - argmin_point_ref[1])*(P_ref[1]/P_ref[2] - argmin_point_ref[1])));
//
            float fac = min_error / (
                (float)MAX_ERROR_STEREO + sqrtf(gradAlongLine) * 20
            );

            cv::Scalar color = cv::Scalar(255*fac, 255-255*fac, 0);// bw

            cv::line(debugImageStereoLines,cv::Point2f(pClose[0], pClose[1]),
                     cv::Point2f(pFar[0], pFar[1]),color,1,8,0);
        }
    }

    result_idepth = idnew_best_match;

    result_eplLength = eplLength;

    return min_error;
}

}
