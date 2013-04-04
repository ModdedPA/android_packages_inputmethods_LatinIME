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

#include "char_utils.h"
#include "dictionary.h"
#include "dic_node_priority_queue.h"
#include "dic_node_vector.h"
#include "dic_traverse_session.h"
#include "proximity_info.h"
#include "scoring.h"
#include "shortcut_utils.h"
#include "suggest.h"
#include "terminal_attributes.h"
#include "traversal.h"
#include "weighting.h"

namespace latinime {

// Initialization of class constants.
const int Suggest::LOOKAHEAD_DIC_NODES_CACHE_SIZE = 25;
const int Suggest::MIN_LEN_FOR_MULTI_WORD_AUTOCORRECT = 16;
const int Suggest::MIN_CONTINUOUS_SUGGESTION_INPUT_SIZE = 2;
const float Suggest::AUTOCORRECT_CLASSIFICATION_THRESHOLD = 0.33f;
const float Suggest::AUTOCORRECT_LANGUAGE_FEATURE_THRESHOLD = 0.6f;

const bool Suggest::CORRECT_SPACE_OMISSION = true;
const bool Suggest::CORRECT_TRANSPOSITION = true;
const bool Suggest::CORRECT_INSERTION = true;
const bool Suggest::CORRECT_OMISSION_G = true;

/**
 * Returns a set of suggestions for the given input touch points. The commitPoint argument indicates
 * whether to prematurely commit the suggested words up to the given point for sentence-level
 * suggestion.
 *
 * Note: Currently does not support concurrent calls across threads. Continuous suggestion is
 * automatically activated for sequential calls that share the same starting input.
 * TODO: Stop detecting continuous suggestion. Start using traverseSession instead.
 */
int Suggest::getSuggestions(ProximityInfo *pInfo, void *traverseSession,
        int *inputXs, int *inputYs, int *times, int *pointerIds, int *inputCodePoints,
        int inputSize, int commitPoint, int *outWords, int *frequencies, int *outputIndices,
        int *outputTypes) const {
    PROF_OPEN;
    PROF_START(0);
    const float maxSpatialDistance = TRAVERSAL->getMaxSpatialDistance();
    DicTraverseSession *tSession = static_cast<DicTraverseSession *>(traverseSession);
    tSession->setupForGetSuggestions(pInfo, inputCodePoints, inputSize, inputXs, inputYs, times,
            pointerIds, maxSpatialDistance, TRAVERSAL->getMaxPointerCount());
    // TODO: Add the way to evaluate cache

    initializeSearch(tSession, commitPoint);
    PROF_END(0);
    PROF_START(1);

    // keep expanding search dicNodes until all have terminated.
    while (tSession->getDicTraverseCache()->activeSize() > 0) {
        expandCurrentDicNodes(tSession);
        tSession->getDicTraverseCache()->advanceActiveDicNodes();
        tSession->getDicTraverseCache()->advanceInputIndex(inputSize);
    }
    PROF_END(1);
    PROF_START(2);
    const int size = outputSuggestions(tSession, frequencies, outWords, outputIndices, outputTypes);
    PROF_END(2);
    PROF_CLOSE;
    return size;
}

/**
 * Initializes the search at the root of the lexicon trie. Note that when possible the search will
 * continue suggestion from where it left off during the last call.
 */
void Suggest::initializeSearch(DicTraverseSession *traverseSession, int commitPoint) const {
    if (!traverseSession->getProximityInfoState(0)->isUsed()) {
        return;
    }
    if (TRAVERSAL->allowPartialCommit()) {
        commitPoint = 0;
    }

    if (traverseSession->getInputSize() > MIN_CONTINUOUS_SUGGESTION_INPUT_SIZE
            && traverseSession->isContinuousSuggestionPossible()) {
        if (commitPoint == 0) {
            // Continue suggestion
            traverseSession->getDicTraverseCache()->continueSearch();
        } else {
            // Continue suggestion after partial commit.
            DicNode *topDicNode =
                    traverseSession->getDicTraverseCache()->setCommitPoint(commitPoint);
            traverseSession->setPrevWordPos(topDicNode->getPrevWordNodePos());
            traverseSession->getDicTraverseCache()->continueSearch();
            traverseSession->setPartiallyCommited();
        }
    } else {
        // Restart recognition at the root.
        traverseSession->resetCache(TRAVERSAL->getMaxCacheSize(), MAX_RESULTS);
        // Create a new dic node here
        DicNode rootNode;
        DicNodeUtils::initAsRoot(traverseSession->getDicRootPos(),
                traverseSession->getOffsetDict(), traverseSession->getPrevWordPos(), &rootNode);
        traverseSession->getDicTraverseCache()->copyPushActive(&rootNode);
    }
}

/**
 * Outputs the final list of suggestions (i.e., terminal nodes).
 */
int Suggest::outputSuggestions(DicTraverseSession *traverseSession, int *frequencies,
        int *outputCodePoints, int *spaceIndices, int *outputTypes) const {
    const int terminalSize = min(MAX_RESULTS,
            static_cast<int>(traverseSession->getDicTraverseCache()->terminalSize()));
    DicNode terminals[MAX_RESULTS]; // Avoiding non-POD variable length array

    for (int index = terminalSize - 1; index >= 0; --index) {
        traverseSession->getDicTraverseCache()->popTerminal(&terminals[index]);
    }

    const float languageWeight = SCORING->getAdjustedLanguageWeight(
            traverseSession, terminals, terminalSize);

    int outputWordIndex = 0;
    // Insert most probable word at index == 0 as long as there is one terminal at least
    const bool hasMostProbableString =
            SCORING->getMostProbableString(traverseSession, terminalSize, languageWeight,
                    &outputCodePoints[0], &outputTypes[0], &frequencies[0]);
    if (hasMostProbableString) {
        ++outputWordIndex;
    }

    // Initial value of the loop index for terminal nodes (words)
    int doubleLetterTerminalIndex = -1;
    DoubleLetterLevel doubleLetterLevel = NOT_A_DOUBLE_LETTER;
    SCORING->searchWordWithDoubleLetter(terminals, terminalSize,
            &doubleLetterTerminalIndex, &doubleLetterLevel);

    int maxScore = S_INT_MIN;
    // Output suggestion results here
    for (int terminalIndex = 0; terminalIndex < terminalSize && outputWordIndex < MAX_RESULTS;
            ++terminalIndex) {
        DicNode *terminalDicNode = &terminals[terminalIndex];
        if (DEBUG_GEO_FULL) {
            terminalDicNode->dump("OUT:");
        }
        const float doubleLetterCost = SCORING->getDoubleLetterDemotionDistanceCost(
                terminalIndex, doubleLetterTerminalIndex, doubleLetterLevel);
        const float compoundDistance = terminalDicNode->getCompoundDistance(languageWeight)
                + doubleLetterCost;
        const TerminalAttributes terminalAttributes(traverseSession->getOffsetDict(),
                terminalDicNode->getFlags(), terminalDicNode->getAttributesPos());
        const int originalTerminalProbability = terminalDicNode->getProbability();

        // Do not suggest words with a 0 probability, or entries that are blacklisted or do not
        // represent a word. However, we should still submit their shortcuts if any.
        const bool isValidWord =
                originalTerminalProbability > 0 && !terminalAttributes.isBlacklistedOrNotAWord();
        // Increase output score of top typing suggestion to ensure autocorrection.
        // TODO: Better integration with java side autocorrection logic.
        // Force autocorrection for obvious long multi-word suggestions.
        const bool isForceCommitMultiWords = TRAVERSAL->allowPartialCommit()
                && (traverseSession->isPartiallyCommited()
                        || (traverseSession->getInputSize() >= MIN_LEN_FOR_MULTI_WORD_AUTOCORRECT
                                && terminalDicNode->hasMultipleWords()));

        const int finalScore = SCORING->calculateFinalScore(
                compoundDistance, traverseSession->getInputSize(),
                isForceCommitMultiWords || (isValidWord && SCORING->doesAutoCorrectValidWord()));

        maxScore = max(maxScore, finalScore);

        if (TRAVERSAL->allowPartialCommit()) {
            // Index for top typing suggestion should be 0.
            if (isValidWord && outputWordIndex == 0) {
                terminalDicNode->outputSpacePositionsResult(spaceIndices);
            }
        }

        // Do not suggest words with a 0 probability, or entries that are blacklisted or do not
        // represent a word. However, we should still submit their shortcuts if any.
        if (isValidWord) {
            outputTypes[outputWordIndex] = Dictionary::KIND_CORRECTION;
            frequencies[outputWordIndex] = finalScore;
            // Populate the outputChars array with the suggested word.
            const int startIndex = outputWordIndex * MAX_WORD_LENGTH;
            terminalDicNode->outputResult(&outputCodePoints[startIndex]);
            ++outputWordIndex;
        }

        const bool sameAsTyped = TRAVERSAL->sameAsTyped(traverseSession, terminalDicNode);
        outputWordIndex = ShortcutUtils::outputShortcuts(&terminalAttributes, outputWordIndex,
                finalScore, outputCodePoints, frequencies, outputTypes, sameAsTyped);
        DicNode::managedDelete(terminalDicNode);
    }

    if (hasMostProbableString) {
        SCORING->safetyNetForMostProbableString(terminalSize, maxScore,
                &outputCodePoints[0], &frequencies[0]);
    }
    return outputWordIndex;
}

/**
 * Expands the dicNodes in the current search priority queue by advancing to the possible child
 * nodes based on the next touch point(s) (or no touch points for lookahead)
 */
void Suggest::expandCurrentDicNodes(DicTraverseSession *traverseSession) const {
    const int inputSize = traverseSession->getInputSize();
    DicNodeVector childDicNodes(TRAVERSAL->getDefaultExpandDicNodeSize());
    DicNode omissionDicNode;

    // TODO: Find more efficient caching
    const bool shouldDepthLevelCache = TRAVERSAL->shouldDepthLevelCache(traverseSession);
    if (shouldDepthLevelCache) {
        traverseSession->getDicTraverseCache()->updateLastCachedInputIndex();
    }
    if (DEBUG_CACHE) {
        AKLOGI("expandCurrentDicNodes depth level cache = %d, inputSize = %d",
                shouldDepthLevelCache, inputSize);
    }
    while (traverseSession->getDicTraverseCache()->activeSize() > 0) {
        DicNode dicNode;
        traverseSession->getDicTraverseCache()->popActive(&dicNode);
        if (dicNode.isTotalInputSizeExceedingLimit()) {
            return;
        }
        childDicNodes.clear();
        const int point0Index = dicNode.getInputIndex(0);
        const bool canDoLookAheadCorrection =
                TRAVERSAL->canDoLookAheadCorrection(traverseSession, &dicNode);
        const bool isLookAheadCorrection = canDoLookAheadCorrection
                && traverseSession->getDicTraverseCache()->
                        isLookAheadCorrectionInputIndex(static_cast<int>(point0Index));
        const bool isCompletion = dicNode.isCompletion(inputSize);

        const bool shouldNodeLevelCache =
                TRAVERSAL->shouldNodeLevelCache(traverseSession, &dicNode);
        if (shouldDepthLevelCache || shouldNodeLevelCache) {
            if (DEBUG_CACHE) {
                dicNode.dump("PUSH_CACHE");
            }
            traverseSession->getDicTraverseCache()->copyPushContinue(&dicNode);
            dicNode.setCached();
        }

        if (isLookAheadCorrection) {
            // The algorithm maintains a small set of "deferred" nodes that have not consumed the
            // latest touch point yet. These are needed to apply look-ahead correction operations
            // that require special handling of the latest touch point. For example, with insertions
            // (e.g., "thiis" -> "this") the latest touch point should not be consumed at all.
            if (CORRECT_TRANSPOSITION) {
                processDicNodeAsTransposition(traverseSession, &dicNode);
            }
            if (CORRECT_INSERTION) {
                processDicNodeAsInsertion(traverseSession, &dicNode);
            }
        } else { // !isLookAheadCorrection
            // Only consider typing error corrections if the normalized compound distance is
            // below a spatial distance threshold.
            // NOTE: the threshold may need to be updated if scoring model changes.
            // TODO: Remove. Do not prune node here.
            const bool allowsErrorCorrections = TRAVERSAL->allowsErrorCorrections(&dicNode);
            // Process for handling space substitution (e.g., hevis => he is)
            if (allowsErrorCorrections
                    && TRAVERSAL->isSpaceSubstitutionTerminal(traverseSession, &dicNode)) {
                createNextWordDicNode(traverseSession, &dicNode, true /* spaceSubstitution */);
            }

            DicNodeUtils::getAllChildDicNodes(
                    &dicNode, traverseSession->getOffsetDict(), &childDicNodes);

            const int childDicNodesSize = childDicNodes.getSizeAndLock();
            for (int i = 0; i < childDicNodesSize; ++i) {
                DicNode *const childDicNode = childDicNodes[i];
                if (isCompletion) {
                    // Handle forward lookahead when the lexicon letter exceeds the input size.
                    processDicNodeAsMatch(traverseSession, childDicNode);
                    continue;
                }
                if (allowsErrorCorrections
                        && TRAVERSAL->isOmission(traverseSession, &dicNode, childDicNode)) {
                    // TODO: (Gesture) Change weight between omission and substitution errors
                    // TODO: (Gesture) Terminal node should not be handled as omission
                    omissionDicNode.initByCopy(childDicNode);
                    processDicNodeAsOmission(traverseSession, &omissionDicNode);
                }
                const ProximityType proximityType = TRAVERSAL->getProximityType(
                        traverseSession, &dicNode, childDicNode);
                switch (proximityType) {
                    // TODO: Consider the difference of proximityType here
                    case MATCH_CHAR:
                    case PROXIMITY_CHAR:
                        processDicNodeAsMatch(traverseSession, childDicNode);
                        break;
                    case ADDITIONAL_PROXIMITY_CHAR:
                        if (allowsErrorCorrections) {
                            processDicNodeAsAdditionalProximityChar(traverseSession, &dicNode,
                                    childDicNode);
                        }
                        break;
                    case SUBSTITUTION_CHAR:
                        if (allowsErrorCorrections) {
                            processDicNodeAsSubstitution(traverseSession, &dicNode, childDicNode);
                        }
                        break;
                    case UNRELATED_CHAR:
                        // Just drop this node and do nothing.
                        break;
                    default:
                        // Just drop this node and do nothing.
                        break;
                }
            }

            // Push the node for look-ahead correction
            if (allowsErrorCorrections && canDoLookAheadCorrection) {
                traverseSession->getDicTraverseCache()->copyPushNextActive(&dicNode);
            }
        }
    }
}

void Suggest::processTerminalDicNode(
        DicTraverseSession *traverseSession, DicNode *dicNode) const {
    if (dicNode->getCompoundDistance() >= static_cast<float>(MAX_VALUE_FOR_WEIGHTING)) {
        return;
    }
    if (!dicNode->isTerminalWordNode()) {
        return;
    }
    if (TRAVERSAL->needsToTraverseAllUserInput()
            && dicNode->getInputIndex(0) < traverseSession->getInputSize()) {
        return;
    }

    if (dicNode->shouldBeFilterdBySafetyNetForBigram()) {
        return;
    }
    // Create a non-cached node here.
    DicNode terminalDicNode;
    DicNodeUtils::initByCopy(dicNode, &terminalDicNode);
    Weighting::addCostAndForwardInputIndex(WEIGHTING, CT_TERMINAL, traverseSession, 0,
            &terminalDicNode, traverseSession->getBigramCacheMap());
    traverseSession->getDicTraverseCache()->copyPushTerminal(&terminalDicNode);
}

/**
 * Adds the expanded dicNode to the next search priority queue. Also creates an additional next word
 * (by the space omission error correction) search path if input dicNode is on a terminal node.
 */
void Suggest::processExpandedDicNode(
        DicTraverseSession *traverseSession, DicNode *dicNode) const {
    processTerminalDicNode(traverseSession, dicNode);
    if (dicNode->getCompoundDistance() < static_cast<float>(MAX_VALUE_FOR_WEIGHTING)) {
        if (TRAVERSAL->isSpaceOmissionTerminal(traverseSession, dicNode)) {
            createNextWordDicNode(traverseSession, dicNode, false /* spaceSubstitution */);
        }
        const int allowsLookAhead = !(dicNode->hasMultipleWords()
                && dicNode->isCompletion(traverseSession->getInputSize()));
        if (dicNode->hasChildren() && allowsLookAhead) {
            traverseSession->getDicTraverseCache()->copyPushNextActive(dicNode);
        }
    }
    DicNode::managedDelete(dicNode);
}

void Suggest::processDicNodeAsMatch(DicTraverseSession *traverseSession,
        DicNode *childDicNode) const {
    weightChildNode(traverseSession, childDicNode);
    processExpandedDicNode(traverseSession, childDicNode);
}

void Suggest::processDicNodeAsAdditionalProximityChar(DicTraverseSession *traverseSession,
        DicNode *dicNode, DicNode *childDicNode) const {
    Weighting::addCostAndForwardInputIndex(WEIGHTING, CT_ADDITIONAL_PROXIMITY,
            traverseSession, dicNode, childDicNode, 0 /* bigramCacheMap */);
    weightChildNode(traverseSession, childDicNode);
    processExpandedDicNode(traverseSession, childDicNode);
}

void Suggest::processDicNodeAsSubstitution(DicTraverseSession *traverseSession,
        DicNode *dicNode, DicNode *childDicNode) const {
    Weighting::addCostAndForwardInputIndex(WEIGHTING, CT_SUBSTITUTION, traverseSession,
            dicNode, childDicNode, 0 /* bigramCacheMap */);
    weightChildNode(traverseSession, childDicNode);
    processExpandedDicNode(traverseSession, childDicNode);
}

/**
 * Handle the dicNode as an omission error (e.g., ths => this). Skip the current letter and consider
 * matches for all possible next letters. Note that just skipping the current letter without any
 * other conditions tends to flood the search dic nodes cache with omission nodes. Instead, check
 * the possible *next* letters after the omission to better limit search to plausible omissions.
 * Note that apostrophes are handled as omissions.
 */
void Suggest::processDicNodeAsOmission(
        DicTraverseSession *traverseSession, DicNode *dicNode) const {
    // If the omission is surely intentional that it should incur zero cost.
    const bool isZeroCostOmission = dicNode->isZeroCostOmission();
    DicNodeVector childDicNodes;

    DicNodeUtils::getAllChildDicNodes(dicNode, traverseSession->getOffsetDict(), &childDicNodes);

    const int size = childDicNodes.getSizeAndLock();
    for (int i = 0; i < size; i++) {
        DicNode *const childDicNode = childDicNodes[i];
        if (!isZeroCostOmission) {
            // Treat this word as omission
            Weighting::addCostAndForwardInputIndex(WEIGHTING, CT_OMISSION, traverseSession,
                    dicNode, childDicNode, 0 /* bigramCacheMap */);
        }
        weightChildNode(traverseSession, childDicNode);

        if (!TRAVERSAL->isPossibleOmissionChildNode(traverseSession, dicNode, childDicNode)) {
            DicNode::managedDelete(childDicNode);
            continue;
        }
        processExpandedDicNode(traverseSession, childDicNode);
    }
}

/**
 * Handle the dicNode as an insertion error (e.g., thiis => this). Skip the current touch point and
 * consider matches for the next touch point.
 */
void Suggest::processDicNodeAsInsertion(DicTraverseSession *traverseSession,
        DicNode *dicNode) const {
    const int16_t pointIndex = dicNode->getInputIndex(0);
    DicNodeVector childDicNodes;
    DicNodeUtils::getProximityChildDicNodes(dicNode, traverseSession->getOffsetDict(),
            traverseSession->getProximityInfoState(0), pointIndex + 1, true, &childDicNodes);
    const int size = childDicNodes.getSizeAndLock();
    for (int i = 0; i < size; i++) {
        DicNode *const childDicNode = childDicNodes[i];
        Weighting::addCostAndForwardInputIndex(WEIGHTING, CT_INSERTION, traverseSession,
                dicNode, childDicNode, 0 /* bigramCacheMap */);
        processExpandedDicNode(traverseSession, childDicNode);
    }
}

/**
 * Handle the dicNode as a transposition error (e.g., thsi => this). Swap the next two touch points.
 */
void Suggest::processDicNodeAsTransposition(DicTraverseSession *traverseSession,
        DicNode *dicNode) const {
    const int16_t pointIndex = dicNode->getInputIndex(0);
    DicNodeVector childDicNodes1;
    DicNodeUtils::getProximityChildDicNodes(dicNode, traverseSession->getOffsetDict(),
            traverseSession->getProximityInfoState(0), pointIndex + 1, false, &childDicNodes1);
    const int childSize1 = childDicNodes1.getSizeAndLock();
    for (int i = 0; i < childSize1; i++) {
        if (childDicNodes1[i]->hasChildren()) {
            DicNodeVector childDicNodes2;
            DicNodeUtils::getProximityChildDicNodes(
                    childDicNodes1[i], traverseSession->getOffsetDict(),
                    traverseSession->getProximityInfoState(0), pointIndex, false, &childDicNodes2);
            const int childSize2 = childDicNodes2.getSizeAndLock();
            for (int j = 0; j < childSize2; j++) {
                DicNode *const childDicNode2 = childDicNodes2[j];
                Weighting::addCostAndForwardInputIndex(WEIGHTING, CT_TRANSPOSITION,
                        traverseSession, childDicNodes1[i], childDicNode2, 0 /* bigramCacheMap */);
                processExpandedDicNode(traverseSession, childDicNode2);
            }
        }
        DicNode::managedDelete(childDicNodes1[i]);
    }
}

/**
 * Weight child node by aligning it to the key
 */
void Suggest::weightChildNode(DicTraverseSession *traverseSession, DicNode *dicNode) const {
    const int inputSize = traverseSession->getInputSize();
    if (dicNode->isCompletion(inputSize)) {
        Weighting::addCostAndForwardInputIndex(WEIGHTING, CT_COMPLETION, traverseSession,
                0 /* parentDicNode */, dicNode, 0 /* bigramCacheMap */);
    } else { // completion
        Weighting::addCostAndForwardInputIndex(WEIGHTING, CT_MATCH, traverseSession,
                0 /* parentDicNode */, dicNode, 0 /* bigramCacheMap */);
    }
}

/**
 * Creates a new dicNode that represents a space insertion at the end of the input dicNode. Also
 * incorporates the unigram / bigram score for the ending word into the new dicNode.
 */
void Suggest::createNextWordDicNode(DicTraverseSession *traverseSession, DicNode *dicNode,
        const bool spaceSubstitution) const {
    if (!TRAVERSAL->isGoodToTraverseNextWord(dicNode)) {
        return;
    }

    // Create a non-cached node here.
    DicNode newDicNode;
    DicNodeUtils::initAsRootWithPreviousWord(traverseSession->getDicRootPos(),
            traverseSession->getOffsetDict(), dicNode, &newDicNode);
    Weighting::addCostAndForwardInputIndex(WEIGHTING, CT_NEW_WORD, traverseSession, dicNode,
            &newDicNode, traverseSession->getBigramCacheMap());
    if (spaceSubstitution) {
        // Merge this with CT_NEW_WORD
        Weighting::addCostAndForwardInputIndex(WEIGHTING, CT_SPACE_SUBSTITUTION,
                traverseSession, 0, &newDicNode, 0 /* bigramCacheMap */);
    }
    traverseSession->getDicTraverseCache()->copyPushNextActive(&newDicNode);
}
} // namespace latinime