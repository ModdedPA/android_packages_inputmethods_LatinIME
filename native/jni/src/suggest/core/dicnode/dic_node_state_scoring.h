/*
 * Copyright (C) 2012 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef LATINIME_DIC_NODE_STATE_SCORING_H
#define LATINIME_DIC_NODE_STATE_SCORING_H

#include <stdint.h>

#include "defines.h"

namespace latinime {

class DicNodeStateScoring {
 public:
    AK_FORCE_INLINE DicNodeStateScoring()
            : mDoubleLetterLevel(NOT_A_DOUBLE_LETTER),
              mEditCorrectionCount(0), mProximityCorrectionCount(0),
              mNormalizedCompoundDistance(0.0f), mSpatialDistance(0.0f), mLanguageDistance(0.0f),
              mTotalPrevWordsLanguageCost(0.0f), mRawLength(0.0f) {
    }

    virtual ~DicNodeStateScoring() {}

    void init() {
        mEditCorrectionCount = 0;
        mProximityCorrectionCount = 0;
        mNormalizedCompoundDistance = 0.0f;
        mSpatialDistance = 0.0f;
        mLanguageDistance = 0.0f;
        mTotalPrevWordsLanguageCost = 0.0f;
        mRawLength = 0.0f;
        mDoubleLetterLevel = NOT_A_DOUBLE_LETTER;
    }

    AK_FORCE_INLINE void init(const DicNodeStateScoring *const scoring) {
        mEditCorrectionCount = scoring->mEditCorrectionCount;
        mProximityCorrectionCount = scoring->mProximityCorrectionCount;
        mNormalizedCompoundDistance = scoring->mNormalizedCompoundDistance;
        mSpatialDistance = scoring->mSpatialDistance;
        mLanguageDistance = scoring->mLanguageDistance;
        mTotalPrevWordsLanguageCost = scoring->mTotalPrevWordsLanguageCost;
        mRawLength = scoring->mRawLength;
        mDoubleLetterLevel = scoring->mDoubleLetterLevel;
    }

    void addCost(const float spatialCost, const float languageCost, const bool doNormalization,
            const int inputSize, const int totalInputIndex, const bool isEditCorrection,
            const bool isProximityCorrection) {
        addDistance(spatialCost, languageCost, doNormalization, inputSize, totalInputIndex);
        if (isEditCorrection) {
            ++mEditCorrectionCount;
        }
        if (isProximityCorrection) {
            ++mProximityCorrectionCount;
        }
        if (languageCost > 0.0f) {
            setTotalPrevWordsLanguageCost(mTotalPrevWordsLanguageCost + languageCost);
        }
    }

    void addRawLength(const float rawLength) {
        mRawLength += rawLength;
    }

    float getCompoundDistance() const {
        return getCompoundDistance(1.0f);
    }

    float getCompoundDistance(const float languageWeight) const {
        return mSpatialDistance + mLanguageDistance * languageWeight;
    }

    float getNormalizedCompoundDistance() const {
        return mNormalizedCompoundDistance;
    }

    float getSpatialDistance() const {
        return mSpatialDistance;
    }

    float getLanguageDistance() const {
        return mLanguageDistance;
    }

    int16_t getEditCorrectionCount() const {
        return mEditCorrectionCount;
    }

    int16_t getProximityCorrectionCount() const {
        return mProximityCorrectionCount;
    }

    float getRawLength() const {
        return mRawLength;
    }

    DoubleLetterLevel getDoubleLetterLevel() const {
        return mDoubleLetterLevel;
    }

    void setDoubleLetterLevel(DoubleLetterLevel doubleLetterLevel) {
        switch(doubleLetterLevel) {
            case NOT_A_DOUBLE_LETTER:
                break;
            case A_DOUBLE_LETTER:
                if (mDoubleLetterLevel != A_STRONG_DOUBLE_LETTER) {
                    mDoubleLetterLevel = doubleLetterLevel;
                }
                break;
            case A_STRONG_DOUBLE_LETTER:
                mDoubleLetterLevel = doubleLetterLevel;
                break;
        }
    }

    float getTotalPrevWordsLanguageCost() const {
        return mTotalPrevWordsLanguageCost;
    }

 private:
    // Caution!!!
    // Use a default copy constructor and an assign operator because shallow copies are ok
    // for this class
    DoubleLetterLevel mDoubleLetterLevel;

    int16_t mEditCorrectionCount;
    int16_t mProximityCorrectionCount;

    float mNormalizedCompoundDistance;
    float mSpatialDistance;
    float mLanguageDistance;
    float mTotalPrevWordsLanguageCost;
    float mRawLength;

    AK_FORCE_INLINE void addDistance(float spatialDistance, float languageDistance,
            bool doNormalization, int inputSize, int totalInputIndex) {
        mSpatialDistance += spatialDistance;
        mLanguageDistance += languageDistance;
        if (!doNormalization) {
            mNormalizedCompoundDistance = mSpatialDistance + mLanguageDistance;
        } else {
            mNormalizedCompoundDistance = (mSpatialDistance + mLanguageDistance)
                    / static_cast<float>(max(1, totalInputIndex));
        }
    }

    //TODO: remove
    AK_FORCE_INLINE void setTotalPrevWordsLanguageCost(float totalPrevWordsLanguageCost) {
        mTotalPrevWordsLanguageCost = totalPrevWordsLanguageCost;
    }
};
} // namespace latinime
#endif // LATINIME_DIC_NODE_STATE_SCORING_H