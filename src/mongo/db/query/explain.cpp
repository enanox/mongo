/**
 *    Copyright (C) 2013-2014 MongoDB Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#include "mongo/platform/basic.h"

#include "mongo/db/query/explain.h"

#include "mongo/db/exec/multi_plan.h"
#include "mongo/db/query/explain_plan.h"
#include "mongo/db/query/get_executor.h"
#include "mongo/db/query/plan_executor.h"
#include "mongo/db/query/query_planner.h"
#include "mongo/db/query/stage_builder.h"
#include "mongo/db/exec/working_set_common.h"
#include "mongo/db/server_options.h"
#include "mongo/db/server_parameters.h"
#include "mongo/util/mongoutils/str.h"
#include "mongo/util/processinfo.h"
#include "mongo/util/version.h"

namespace {

    using namespace mongo;

    /**
     * Traverse the tree rooted at 'root', and add all tree nodes into the list 'flattened'.
     */
    void flattenStatsTree(PlanStageStats* root, vector<PlanStageStats*>* flattened) {
        flattened->push_back(root);
        for (size_t i = 0; i < root->children.size(); ++i) {
            flattenStatsTree(root->children[i], flattened);
        }
    }

    /**
     * Traverse the tree rooted at 'root', and add all nodes into the list 'flattened'.
     */
    void flattenExecTree(PlanStage* root, vector<PlanStage*>* flattened) {
        flattened->push_back(root);
        vector<PlanStage*> children = root->getChildren();
        for (size_t i = 0; i < children.size(); ++i) {
            flattenExecTree(children[i], flattened);
        }
    }

    /**
     * Get a pointer to the MultiPlanStage inside the stage tree rooted at 'root'.
     * Returns NULL if there is no MPS.
     */
    MultiPlanStage* getMultiPlanStage(PlanStage* root) {
        if (root->stageType() == STAGE_MULTI_PLAN) {
            MultiPlanStage* mps = static_cast<MultiPlanStage*>(root);
            return mps;
        }

        vector<PlanStage*> children = root->getChildren();
        for (size_t i = 0; i < children.size(); i++) {
            MultiPlanStage* mps = getMultiPlanStage(children[i]);
            if (mps != NULL) {
                return mps;
            }
        }

        return NULL;
    }

    /**
     * Given the SpecificStats object for a stage and the type of the stage, returns the
     * number of index keys examined by the stage.
     *
     * This is used for getting the total number of keys examined by a plan. We need
     * to collect a 'totalKeysExamined' metric for a regular explain (in which case this
     * gets called from Explain::generateExecStats()) or for the slow query log / profiler
     * (in which case this gets called from Explain::getSummaryStats()).
     */
     size_t getKeysExamined(StageType type, const SpecificStats* specific) {
        if (STAGE_IXSCAN == type) {
            const IndexScanStats* spec = static_cast<const IndexScanStats*>(specific);
            return spec->keysExamined;
        }
        else if (STAGE_IDHACK == type) {
            const IDHackStats* spec = static_cast<const IDHackStats*>(specific);
            return spec->keysExamined;
        }
        else if (STAGE_TEXT == type) {
            const TextStats* spec = static_cast<const TextStats*>(specific);
            return spec->keysExamined;
        }
        else if (STAGE_COUNT == type) {
            const CountStats* spec = static_cast<const CountStats*>(specific);
            return spec->keysExamined;
        }
        else if (STAGE_DISTINCT == type) {
            const DistinctScanStats* spec = static_cast<const DistinctScanStats*>(specific);
            return spec->keysExamined;
        }

        return 0;
     }

    /**
     * Given the SpecificStats object for a stage and the type of the stage, returns the
     * number of documents examined by the stage.
     *
     * This is used for getting the total number of documents examined by a plan. We need
     * to collect a 'totalDocsExamined' metric for a regular explain (in which case this
     * gets called from Explain::generateExecStats()) or for the slow query log / profiler
     * (in which case this gets called from Explain::getSummaryStats()).
     */
    size_t getDocsExamined(StageType type, const SpecificStats* specific) {
        if (STAGE_IDHACK == type) {
            const IDHackStats* spec = static_cast<const IDHackStats*>(specific);
            return spec->docsExamined;
        }
        else if (STAGE_TEXT == type) {
            const TextStats* spec = static_cast<const TextStats*>(specific);
            return spec->fetches;
        }
        else if (STAGE_FETCH == type) {
            const FetchStats* spec = static_cast<const FetchStats*>(specific);
            return spec->docsExamined;
        }
        else if (STAGE_COLLSCAN == type) {
            const CollectionScanStats* spec = static_cast<const CollectionScanStats*>(specific);
            return spec->docsTested;
        }

        return 0;
    }

    /**
     * Adds to the plan summary string being built by 'ss' for the execution stage 'stage'.
     */
    void addStageSummaryStr(PlanStage* stage, mongoutils::str::stream& ss) {
        // First add the stage type string.
        const CommonStats* common = stage->getCommonStats();
        ss << common->stageTypeStr;

        // Some leaf nodes also provide info about the index they used.
        const SpecificStats* specific = stage->getSpecificStats();
        if (STAGE_COUNT == stage->stageType()) {
            const CountStats* spec = static_cast<const CountStats*>(specific);
            ss << " " << spec->keyPattern;
        }
        else if (STAGE_DISTINCT == stage->stageType()) {
            const DistinctScanStats* spec = static_cast<const DistinctScanStats*>(specific);
            ss << " " << spec->keyPattern;
        }
        else if (STAGE_GEO_NEAR_2D == stage->stageType()) {
            const NearStats* spec = static_cast<const NearStats*>(specific);
            ss << " " << spec->keyPattern;
        }
        else if (STAGE_GEO_NEAR_2DSPHERE == stage->stageType()) {
            const NearStats* spec = static_cast<const NearStats*>(specific);
            ss << " " << spec->keyPattern;
        }
        else if (STAGE_IXSCAN == stage->stageType()) {
            const IndexScanStats* spec = static_cast<const IndexScanStats*>(specific);
            ss << " " << spec->keyPattern;
        }
        else if (STAGE_TEXT == stage->stageType()) {
            const TextStats* spec = static_cast<const TextStats*>(specific);
            ss << " " << spec->indexPrefix;
        }
    }

} // namespace

namespace mongo {

    using mongoutils::str::stream;

    MONGO_EXPORT_SERVER_PARAMETER(enableNewExplain, bool, false);

    // static
    void Explain::statsToBSON(const PlanStageStats& stats,
                              Explain::Verbosity verbosity,
                              BSONObjBuilder* bob,
                              BSONObjBuilder* topLevelBob) {
        invariant(bob);
        invariant(topLevelBob);

        // Stop as soon as the BSON object we're building exceeds 10 MB.
        static const int kMaxStatsBSONSize = 10 * 1024 * 1024;
        if (topLevelBob->len() > kMaxStatsBSONSize) {
            bob->append("warning", "stats tree exceeded 10 MB");
            return;
        }

        // Stage name.
        bob->append("stage", stats.common.stageTypeStr);

        // Display the BSON representation of the filter, if there is one.
        if (!stats.common.filter.isEmpty()) {
            bob->append("filter", stats.common.filter);
        }

        // Some top-level exec stats get pulled out of the root stage.
        if (verbosity >= Explain::EXEC_STATS) {
            bob->appendNumber("nReturned", stats.common.advanced);
            bob->appendNumber("executionTimeMillis", stats.common.executionTimeMillis);
        }

        // At full verbosity, we add some extra common details.
        if (verbosity == Explain::FULL) {
            bob->appendNumber("works", stats.common.works);
            bob->appendNumber("advanced", stats.common.advanced);
            bob->appendNumber("needTime", stats.common.needTime);
            bob->appendNumber("isEOF", stats.common.isEOF);
            bob->appendNumber("invalidates", stats.common.invalidates);
        }

        // Stage-specific stats
        if (STAGE_AND_HASH == stats.stageType) {
            AndHashStats* spec = static_cast<AndHashStats*>(stats.specific.get());

            if (verbosity >= Explain::EXEC_STATS) {
                bob->appendNumber("memUsage", spec->memUsage);
                bob->appendNumber("memLimit", spec->memLimit);
            }

            // Extra info at full verbosity.
            if (verbosity == Explain::FULL) {
                bob->appendNumber("flaggedButPassed", spec->flaggedButPassed);
                bob->appendNumber("flaggedInProgress", spec->flaggedInProgress);
                for (size_t i = 0; i < spec->mapAfterChild.size(); ++i) {
                    bob->appendNumber(string(stream() << "mapAfterChild_" << i),
                                      spec->mapAfterChild[i]);
                }
            }
        }
        else if (STAGE_AND_SORTED == stats.stageType) {
            AndSortedStats* spec = static_cast<AndSortedStats*>(stats.specific.get());

            // Extra info at full verbosity.
            if (verbosity == Explain::FULL) {
                bob->appendNumber("flagged", spec->flagged);
                bob->appendNumber("matchTested", spec->matchTested);
                for (size_t i = 0; i < spec->failedAnd.size(); ++i) {
                    bob->appendNumber(string(stream() << "failedAnd_" << i),
                                      spec->failedAnd[i]);
                }
            }
        }
        else if (STAGE_COLLSCAN == stats.stageType) {
            CollectionScanStats* spec = static_cast<CollectionScanStats*>(stats.specific.get());
            if (verbosity >= Explain::EXEC_STATS) {
                bob->appendNumber("docsExamined", spec->docsTested);
            }
        }
        else if (STAGE_COUNT == stats.stageType) {
            CountStats* spec = static_cast<CountStats*>(stats.specific.get());

            if (verbosity >= Explain::EXEC_STATS) {
                bob->appendNumber("keysExamined", spec->keysExamined);
            }

            bob->append("keyPattern", spec->keyPattern);
            bob->appendBool("isMultiKey", spec->isMultiKey);
        }
        else if (STAGE_FETCH == stats.stageType) {
            FetchStats* spec = static_cast<FetchStats*>(stats.specific.get());
            if (verbosity >= Explain::EXEC_STATS) {
                bob->appendNumber("docsExamined", spec->docsExamined);
            }

            // Extra info at full verbosity.
            if (verbosity == Explain::FULL) {
                bob->appendNumber("alreadyHasObj", spec->alreadyHasObj);
            }
        }
        else if (STAGE_IDHACK == stats.stageType) {
            IDHackStats* spec = static_cast<IDHackStats*>(stats.specific.get());
            if (verbosity >= Explain::EXEC_STATS) {
                bob->appendNumber("keysExamined", spec->keysExamined);
                bob->appendNumber("docsExamined", spec->docsExamined);
            }
        }
        else if (STAGE_IXSCAN == stats.stageType) {
            IndexScanStats* spec = static_cast<IndexScanStats*>(stats.specific.get());

            if (verbosity >= Explain::EXEC_STATS) {
                bob->appendNumber("keysExamined", spec->keysExamined);
            }

            bob->append("keyPattern", spec->keyPattern);
            bob->appendBool("isMultiKey", spec->isMultiKey);

            // Bounds can get large. Truncate to 1 MB.
            static const int kMaxBoundsSize = 1024 * 1024;
            bob->append("indexBounds", spec->indexBoundsVerbose.substr(0, kMaxBoundsSize));

            // Extra info at full verbosity.
            if (verbosity == Explain::FULL) {
                bob->appendNumber("dupsTested", spec->dupsTested);
                bob->appendNumber("dupsDropped", spec->dupsDropped);
                bob->appendNumber("seenInvalidated", spec->seenInvalidated);
                bob->appendNumber("matchTested", spec->matchTested);
            }
        }
        else if (STAGE_OR == stats.stageType) {
            OrStats* spec = static_cast<OrStats*>(stats.specific.get());

            // Extra info at full verbosity.
            if (verbosity == Explain::FULL) {
                bob->appendNumber("dupsTested", spec->dupsTested);
                bob->appendNumber("dupsDropped", spec->dupsDropped);
                bob->appendNumber("locsForgotten", spec->locsForgotten);
                for (size_t i = 0; i < spec->matchTested.size(); ++i) {
                    bob->appendNumber(string(stream() << "matchTested_" << i),
                                      spec->matchTested[i]);
                }
            }
        }
        else if (STAGE_LIMIT == stats.stageType) {
            LimitStats* spec = static_cast<LimitStats*>(stats.specific.get());
            bob->appendNumber("limitAmount", spec->limit);
        }
        else if (STAGE_PROJECTION == stats.stageType) {
            ProjectionStats* spec = static_cast<ProjectionStats*>(stats.specific.get());
            bob->append("transformBy", spec->projObj);
        }
        else if (STAGE_SHARDING_FILTER == stats.stageType) {
            ShardingFilterStats* spec = static_cast<ShardingFilterStats*>(stats.specific.get());

            if (verbosity >= Explain::EXEC_STATS) {
                bob->appendNumber("chunkSkips", spec->chunkSkips);
            }
        }
        else if (STAGE_SKIP == stats.stageType) {
            SkipStats* spec = static_cast<SkipStats*>(stats.specific.get());
            bob->appendNumber("skipAmount", spec->skip);
        }
        else if (STAGE_SORT == stats.stageType) {
            SortStats* spec = static_cast<SortStats*>(stats.specific.get());
            bob->append("sortPattern", spec->sortPattern);

            if (verbosity >= Explain::EXEC_STATS) {
                bob->appendNumber("memUsage", spec->memUsage);
                bob->appendNumber("memLimit", spec->memLimit);
            }

            if (spec->limit > 0) {
                bob->appendNumber("limitAmount", spec->limit);
            }
        }
        else if (STAGE_SORT_MERGE == stats.stageType) {
            MergeSortStats* spec = static_cast<MergeSortStats*>(stats.specific.get());
            bob->append("sortPattern", spec->sortPattern);

            // Extra info at full verbosity.
            if (verbosity == Explain::FULL) {
                bob->appendNumber("dupsTested", spec->dupsTested);
                bob->appendNumber("dupsDropped", spec->dupsDropped);
            }
        }
        else if (STAGE_TEXT == stats.stageType) {
            TextStats* spec = static_cast<TextStats*>(stats.specific.get());

            if (verbosity >= Explain::EXEC_STATS) {
                bob->appendNumber("keysExamined", spec->keysExamined);
                bob->appendNumber("docsExamined", spec->fetches);
            }

            bob->append("indexPrefix", spec->indexPrefix);
            bob->append("parsedTextQuery", spec->parsedTextQuery);
        }

        // We're done if there are no children.
        if (stats.children.empty()) {
            return;
        }

        // If there's just one child (a common scenario), avoid making an array. This makes
        // the output more readable by saving a level of nesting. Name the field 'inputStage'
        // rather than 'inputStages'.
        if (1 == stats.children.size()) {
            BSONObjBuilder childBob;
            statsToBSON(*stats.children[0], verbosity, &childBob, topLevelBob);
            bob->append("inputStage", childBob.obj());
            return;
        }

        // There is more than one child. Recursively call statsToBSON(...) on each
        // of them and add them to the 'inputStages' array.

        BSONArrayBuilder childrenBob(bob->subarrayStart("inputStages"));
        for (size_t i = 0; i < stats.children.size(); ++i) {
            BSONObjBuilder childBob(childrenBob.subobjStart());
            statsToBSON(*stats.children[i], verbosity, &childBob, topLevelBob);
        }
        childrenBob.doneFast();
    }

    // static
    BSONObj Explain::statsToBSON(const PlanStageStats& stats,
                                 Explain::Verbosity verbosity) {
        BSONObjBuilder bob;
        statsToBSON(stats, &bob, verbosity);
        return bob.obj();
    }

    // static
    void Explain::statsToBSON(const PlanStageStats& stats,
                              BSONObjBuilder* bob,
                              Explain::Verbosity verbosity) {
        statsToBSON(stats, verbosity, bob, bob);
    }

    // static
    void Explain::generatePlannerInfo(CanonicalQuery* query,
                                      PlanStageStats* winnerStats,
                                      const vector<PlanStageStats*>& rejectedStats,
                                      BSONObjBuilder* out) {
        BSONObjBuilder plannerBob(out->subobjStart("queryPlanner"));;

        plannerBob.append("plannerVersion", QueryPlanner::kPlannerVersion);

        // In general we should have a canonical query, but sometimes we may avoid
        // creating a canonical query as an optimization (specifically, the update system
        // does not canonicalize for idhack updates). In these cases, 'query' is NULL.
        if (NULL != query) {
            BSONObjBuilder parsedQueryBob(plannerBob.subobjStart("parsedQuery"));
            query->root()->toBSON(&parsedQueryBob);
            parsedQueryBob.doneFast();
        }

        BSONObjBuilder winningPlanBob(plannerBob.subobjStart("winningPlan"));
        statsToBSON(*winnerStats, &winningPlanBob, Explain::QUERY_PLANNER);
        winningPlanBob.doneFast();

        // Genenerate array of rejected plans.
        BSONArrayBuilder allPlansBob(plannerBob.subarrayStart("rejectedPlans"));
        for (size_t i = 0; i < rejectedStats.size(); i++) {
            BSONObjBuilder childBob(allPlansBob.subobjStart());
            statsToBSON(*rejectedStats[i], &childBob, Explain::QUERY_PLANNER);
        }
        allPlansBob.doneFast();

        plannerBob.doneFast();
    }

    // static
    void Explain::generateExecStats(PlanStageStats* stats,
                                    Explain::Verbosity verbosity,
                                    BSONObjBuilder* out) {

        out->appendNumber("nReturned", stats->common.advanced);
        out->appendNumber("executionTimeMillis", stats->common.executionTimeMillis);

        // Flatten the stats tree into a list.
        vector<PlanStageStats*> statsNodes;
        flattenStatsTree(stats, &statsNodes);

        // Iterate over all stages in the tree and get the total number of keys/docs examined.
        // These are just aggregations of information already available in the stats tree.
        size_t totalKeysExamined = 0;
        size_t totalDocsExamined = 0;
        for (size_t i = 0; i < statsNodes.size(); ++i) {

            totalKeysExamined += getKeysExamined(statsNodes[i]->stageType,
                                                 statsNodes[i]->specific.get());
            totalDocsExamined += getDocsExamined(statsNodes[i]->stageType,
                                                 statsNodes[i]->specific.get());
        }

        out->appendNumber("totalKeysExamined", totalKeysExamined);
        out->appendNumber("totalDocsExamined", totalDocsExamined);

        // Add the tree of stages, with individual execution stats for each stage.
        BSONObjBuilder stagesBob(out->subobjStart("executionStages"));
        statsToBSON(*stats, &stagesBob, verbosity);
        stagesBob.doneFast();
    }

    // static
    void Explain::generateServerInfo(BSONObjBuilder* out) {
        BSONObjBuilder serverBob(out->subobjStart("serverInfo"));
        out->append("host", getHostNameCached());
        out->appendNumber("port", serverGlobalParams.port);
        out->append("version", versionString);
        out->append("gitVersion", gitVersion());

        ProcessInfo p;
        BSONObjBuilder bOs;
        bOs.append("type", p.getOsType());
        bOs.append("name", p.getOsName());
        bOs.append("version", p.getOsVersion());
        serverBob.append(StringData("os"), bOs.obj());

        serverBob.doneFast();
    }

    // static
    void Explain::explainCountEmptyQuery(BSONObjBuilder* out) {
        BSONObjBuilder plannerBob(out->subobjStart("queryPlanner"));

        plannerBob.append("plannerVersion", QueryPlanner::kPlannerVersion);

        plannerBob.append("winningPlan", "TRIVIAL_COUNT");

        // Empty array of rejected plans.
        BSONArrayBuilder allPlansBob(plannerBob.subarrayStart("rejectedPlans"));
        allPlansBob.doneFast();

        plannerBob.doneFast();

        generateServerInfo(out);
    }

    // static
    Status Explain::explainStages(PlanExecutor* exec,
                                  Explain::Verbosity verbosity,
                                  BSONObjBuilder* out) {
        //
        // Step 1: run the stages as required by the verbosity level.
        //

        // Inspect the tree to see if there is a MultiPlanStage.
        MultiPlanStage* mps = getMultiPlanStage(exec->getRootStage());

        // The executionStats verbosity level requires that we run the winning plan
        // until if finishes.
        if (verbosity >= Explain::EXEC_STATS) {
            Status s = exec->executePlan();
            if (!s.isOK()) {
                return s;
            }
        }

        // The allPlansExecution verbosity level requires that we run all plans to completion,
        // if there are multiple candidates. If 'mps' is NULL, then there was only one candidate,
        // and we don't have to worry about gathering stats for rejected plans.
        if (verbosity == Explain::EXEC_ALL_PLANS && NULL != mps) {
            Status s = mps->executeAllPlans();
            if (!s.isOK()) {
                return s;
            }
        }

        //
        // Step 2: collect plan stats (which also give the structure of the plan tree).
        //

        // Get stats for the winning plan.
        scoped_ptr<PlanStageStats> winningStats(exec->getStats());

        // Get stats for the rejected plans, if there were rehected plans.
        vector<PlanStageStats*> rejectedStats;
        if (NULL != mps) {
            rejectedStats = mps->generateCandidateStats();
        }

        //
        // Step 3: use the stats trees to produce explain BSON.
        //

        CanonicalQuery* query = exec->getCanonicalQuery();
        if (verbosity >= Explain::QUERY_PLANNER) {
            generatePlannerInfo(query, winningStats.get(), rejectedStats, out);
        }

        if (verbosity >= Explain::EXEC_STATS) {
            BSONObjBuilder execBob(out->subobjStart("executionStats"));

            // Generate exec stats BSON for the winning plan.
            generateExecStats(winningStats.get(), verbosity, &execBob);

            // Also generate exec stats for each rejected plan, if the verbosity level
            // is high enough.
            if (verbosity >= Explain::EXEC_ALL_PLANS) {
                BSONArrayBuilder rejectedBob(execBob.subarrayStart("rejectedPlansExecution"));
                for (size_t i = 0; i < rejectedStats.size(); ++i) {
                    BSONObjBuilder planBob(rejectedBob.subobjStart());
                    generateExecStats(rejectedStats[i], verbosity, &planBob);
                    planBob.doneFast();
                }
                rejectedBob.doneFast();
            }

            execBob.doneFast();
        }

        generateServerInfo(out);

        return Status::OK();
    }

    // static
    string Explain::getPlanSummary(PlanStage* root) {
        vector<PlanStage*> stages;
        flattenExecTree(root, &stages);

        // Use this stream to build the plan summary string.
        mongoutils::str::stream ss;
        bool seenLeaf = false;

        for (size_t i = 0; i < stages.size(); i++) {
            if (stages[i]->getChildren().empty()) {
                // This is a leaf node. Add to the plan summary string accordingly. Unless
                // this is the first leaf we've seen, add a delimiting string first.
                if (seenLeaf) {
                    ss << ", ";
                }
                else {
                    seenLeaf = true;
                }
                addStageSummaryStr(stages[i], ss);
            }
        }

        return ss;
    }

    // static
    void Explain::getSummaryStats(PlanExecutor* exec, PlanSummaryStats* statsOut) {
        invariant(NULL != statsOut);

        PlanStage* root = exec->getRootStage();

        // We can get some of the fields we need from the common stats stored in the
        // root stage of the plan tree.
        const CommonStats* common = root->getCommonStats();
        statsOut->nReturned = common->advanced;
        statsOut->executionTimeMillis = common->executionTimeMillis;

        // Generate the plan summary string.
        statsOut->summaryStr = getPlanSummary(root);

        // The other fields are aggregations over the stages in the plan tree. We flatten
        // the tree into a list and then compute these aggregations.
        vector<PlanStage*> stages;
        flattenExecTree(root, &stages);

        for (size_t i = 0; i < stages.size(); i++) {
            statsOut->totalKeysExamined += getKeysExamined(stages[i]->stageType(),
                                                           stages[i]->getSpecificStats());
            statsOut->totalDocsExamined += getDocsExamined(stages[i]->stageType(),
                                                           stages[i]->getSpecificStats());

            if (STAGE_IDHACK == stages[i]->stageType()) {
                statsOut->isIdhack = true;
            }
            if (STAGE_SORT == stages[i]->stageType()) {
                statsOut->hasSortStage = true;
            }
        }
    }

    // TODO: This is temporary and should get deleted. There are a few small ways in which
    // this differs from 2.6 explain, but I'm not too worried because this entire format is
    // going away soon:
    //  1) 'indexBounds' field excluded from idhack explain.
    //  2) 'filterSet' field (for index filters) excluded.
    Status Explain::legacyExplain(PlanExecutor* exec, TypeExplain** explain) {
        invariant(exec);
        invariant(explain);

        scoped_ptr<PlanStageStats> stats(exec->getStats());
        if (NULL == stats.get()) {
            return Status(ErrorCodes::InternalError, "no stats available to explain plan");
        }

        // Special explain format for EOF.
        if (STAGE_EOF == stats->stageType) {
            *explain = new TypeExplain();

            // Fill in mandatory fields.
            (*explain)->setN(0);
            (*explain)->setNScannedObjects(0);
            (*explain)->setNScanned(0);

            // Fill in all the main fields that don't have a default in the explain data structure.
            (*explain)->setCursor("BasicCursor");
            (*explain)->setScanAndOrder(false);
            (*explain)->setIsMultiKey(false);
            (*explain)->setIndexOnly(false);
            (*explain)->setNYields(0);
            (*explain)->setNChunkSkips(0);

            TypeExplain* allPlans = new TypeExplain;
            allPlans->setCursor("BasicCursor");
            (*explain)->addToAllPlans(allPlans); // ownership xfer

            (*explain)->setNScannedObjectsAllPlans(0);
            (*explain)->setNScannedAllPlans(0);

            return Status::OK();
        }

        // Special explain format for idhack.
        vector<PlanStageStats*> statNodes;
        flattenStatsTree(stats.get(), &statNodes);
        PlanStageStats* idhack = NULL;
        for (size_t i = 0; i < statNodes.size(); i++) {
            if (STAGE_IDHACK == statNodes[i]->stageType) {
                idhack = statNodes[i];
                break;
            }
        }

        if (NULL != idhack) {
            // Explain format does not match 2.4 and is intended
            // to indicate clearly that the ID hack has been applied.
            *explain = new TypeExplain();

            IDHackStats* idhackStats = static_cast<IDHackStats*>(idhack->specific.get());

            (*explain)->setCursor("IDCursor");
            (*explain)->setIDHack(true);
            (*explain)->setN(stats->common.advanced);
            (*explain)->setNScanned(idhackStats->keysExamined);
            (*explain)->setNScannedObjects(idhackStats->docsExamined);

            return Status::OK();
        }

        Status status = explainPlan(*stats, explain, true /* full details */);
        if (!status.isOK()) {
            return status;
        }

        // Fill in explain fields that are accounted by on the runner level.
        TypeExplain* chosenPlan = NULL;
        explainPlan(*stats, &chosenPlan, false /* no full details */);
        if (chosenPlan) {
            (*explain)->addToAllPlans(chosenPlan);
        }
        (*explain)->setNScannedObjectsAllPlans((*explain)->getNScannedObjects());
        (*explain)->setNScannedAllPlans((*explain)->getNScanned());

        return Status::OK();
    }

} // namespace mongo
