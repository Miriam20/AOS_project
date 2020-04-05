/*
 * Copyright (C) 2016  Politecnico di Milano
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "adaptive_cpu_schedpol.h"

#include <cstdlib>
#include <cstdint>
#include <iostream>

#include "bbque/modules_factory.h"
#include "bbque/utils/logging/logger.h"

#include "bbque/app/working_mode.h"
#include "bbque/res/binder.h"

#include "bbque/binding_manager.h"

#define MODULE_CONFIG SCHEDULER_POLICY_CONFIG "." SCHEDULER_POLICY_NAME

#define INITIAL_DEFAULT_QUOTA 100
#define ADMISSIBLE_DELTA 10
#define QUOTA_EXPANSION_TERM 0.2
#define QUOTA_REDUCTION_TERM 2

using namespace std::placeholders;

namespace bu = bbque::utils;
namespace po = boost::program_options;

namespace bbque { namespace plugins {
    

// :::::::::::::::::::::: Static plugin interface ::::::::::::::::::::::::::::

void * Adaptive_cpuSchedPol::Create(PF_ObjectParams *) {
    return new Adaptive_cpuSchedPol();
    }

    int32_t Adaptive_cpuSchedPol::Destroy(void * plugin) {
    if (!plugin)
        return -1;
    delete (Adaptive_cpuSchedPol *)plugin;
    return 0;
    }

    // ::::::::::::::::::::: Scheduler policy module interface :::::::::::::::::::

    char const * Adaptive_cpuSchedPol::Name() {
    return SCHEDULER_POLICY_NAME;
    }

    Adaptive_cpuSchedPol::Adaptive_cpuSchedPol():
        cm(ConfigurationManager::GetInstance()),
        ra(ResourceAccounter::GetInstance()) {
    logger = bu::Logger::GetLogger(MODULE_NAMESPACE);
    assert(logger);
    if (logger)
        logger->Info("adaptive_cpu: Built a new dynamic object[%p]", this);
    else
        fprintf(stderr,
            FI("adaptive_cpu: Built new dynamic object [%p]\n"), (void *)this);
    }


    Adaptive_cpuSchedPol::~Adaptive_cpuSchedPol() {

}


SchedulerPolicyIF::ExitCode_t Adaptive_cpuSchedPol::_Init() {
    // Processing elements IDs
    auto & resource_types = sys->ResourceTypes();
    auto const & r_ids_entry = resource_types.find(br::ResourceType::PROC_ELEMENT);
    pe_ids = r_ids_entry->second;
    logger->Info("Init: %d processing elements available", pe_ids.size());
    if (pe_ids.empty()) {
        logger->Crit("Init: not available CPU cores!");
        return SCHED_R_UNAVAILABLE;
    }

    // Applications count
    nr_apps = sys->SchedulablesCount(ba::Schedulable::READY);
    nr_apps += sys->SchedulablesCount(ba::Schedulable::RUNNING);
    logger->Info("Init: nr. active applications = %d", nr_apps);

    return SCHED_OK;
}




/********************************
************ MY CODE*************
********************************/


void Adaptive_cpuSchedPol::ComputeQuota(AppInfo_t * ainfo)
{
    std::string desc;
    uint64_t coef1, coef2;
    uint64_t surplus = 0;
    uint64_t slack = 0;
    
    if (ainfo->pawm == nullptr){
        coef1 = 0;
        coef2 = 0;
    
        logger->Info("Assigning quota first round");
        
        desc = "Default";

        if (ainfo->available < INITIAL_DEFAULT_QUOTA)
            ainfo->next_quota = ainfo->available;
        else           
            ainfo->next_quota = INITIAL_DEFAULT_QUOTA;
        }
    
    else if (ainfo->prev_delta > ADMISSIBLE_DELTA){
        coef1 = 0;
        coef2 = 1;
        
        logger->Info("Reducing unused quota");
        
        surplus = ainfo->prev_delta/QUOTA_REDUCTION_TERM;
        desc = "Decreased";
    }
    else if (ainfo->prev_delta == 0) {
        coef1 = 1;
        coef2 = 0;
        
        logger->Info("Increasing quota");

        if (ainfo->available < ainfo->prev_quota*QUOTA_EXPANSION_TERM)
            slack = ainfo->available;
        else
            slack = ainfo->prev_quota*QUOTA_EXPANSION_TERM;
        desc = "Enhanced";
    }
    else {
        coef1 = 0;
        coef2 = 0;
        logger->Info("Confirming previously assigned quota");
        desc = "Stable";
    }
    
    //Unique formula to compute quota
    ainfo->next_quota = ainfo->prev_quota + coef1*slack - coef2*surplus;
    
    ainfo->pawm = std::make_shared<ba::WorkingMode>(
        ainfo->papp->WorkingModes().size(), desc, 1, ainfo->papp);
    
    assert(ainfo->pawm);
    

    logger->Info("Assigned quota=%d, Previous quota=%d, Previously used CPU=%d, Delta=%d Available cpu=%d",
            ainfo->next_quota,
            ainfo->prev_quota,
            ainfo->prev_used,
            ainfo->prev_delta,
            ainfo->available);
}


AppInfo_t Adaptive_cpuSchedPol::InitializeAppInfo(bbque::app::AppCPtr_t papp){
    AppInfo_t ainfo;
    
    ainfo.papp = papp;
    ainfo.pawm = papp->CurrentAWM();
    ainfo.prev_quota = ra.UsedBy(
        "sys.cpu.pe",
        papp,
        0);
    auto prof = papp->GetRuntimeProfile();
    ainfo.prev_used = prof.cpu_usage;
    ainfo.prev_delta = ainfo.prev_quota - ainfo.prev_used;
    ainfo.available = ra.Available("sys.cpu.pe");
    
    return ainfo;
}
    

SchedulerPolicyIF::ExitCode_t
Adaptive_cpuSchedPol::AssignWorkingMode(bbque::app::AppCPtr_t papp)
{  
    ApplicationManager & am(ApplicationManager::GetInstance());

    if (papp == nullptr) {
        logger->Error("AssignWorkingMode: null application descriptor!");
        return SCHED_ERROR;
    }

    // Print the run-time profiling info if running
    if (papp->Running()) {
        auto prof = papp->GetRuntimeProfile();
        logger->Info("AssignWorkingMode: [%s] "
            "cpu_usage=%d c_time=%d, ggap=%d [valid=%d]",
            papp->StrId(),
            prof.cpu_usage,
            prof.ctime_ms,
            prof.ggap_percent,
            prof.is_valid);
    }
    
    /*if (papp->Running()){
        bbque::app::AwmPtr_t pawm = papp->CurrentAWM();
        papp->CurrentAWM()->ClearResourceRequests();
//         papp->CurrentAWM()->ClearResourceBinding();
    }
    */
    AppInfo_t ainfo = Adaptive_cpuSchedPol::InitializeAppInfo(papp);
    
    Adaptive_cpuSchedPol::ComputeQuota(&ainfo);
    
    auto pawm = ainfo.pawm;
    
    pawm->AddResourceRequest(
        "sys.cpu.pe",
        ainfo.next_quota,
        br::ResourceAssignment::Policy::BALANCED);
    
    // Look for the first available CPU
    BindingManager & bdm(BindingManager::GetInstance());
    BindingMap_t & bindings(bdm.GetBindingDomains());
    auto & cpu_ids(bindings[br::ResourceType::CPU]->r_ids);
    
    for (BBQUE_RID_TYPE cpu_id : cpu_ids) {
        logger->Info("AssingWorkingMode: [%s] binding attempt CPU id = %d",
        papp->StrId(), cpu_id);
        
        
        // CPU binding

        int32_t ref_num = -1;
        ref_num = pawm->BindResource(br::ResourceType::CPU, R_ID_ANY, cpu_id, ref_num);

        if (ref_num < 0) {
            logger->Error("AssingWorkingMode: [%s] CPU binding to < %d > failed",
                papp->StrId(), cpu_id);
            continue;
        }
        
        // Schedule request
        ApplicationManager::ExitCode_t am_ret;
        am_ret = am.ScheduleRequest(papp, pawm, sched_status_view, ref_num);
        if (am_ret != ApplicationManager::AM_SUCCESS) {
            logger->Error("AssignWorkingMode: [%s] schedule request failed",
                papp->StrId());
            continue;
        }
        
        return SCHED_OK;
    }
    
    return SCHED_ERROR;

}

/********************************
************ MY CODE*************
********************************/




SchedulerPolicyIF::ExitCode_t
Adaptive_cpuSchedPol::Schedule(
        System & system,
        RViewToken_t & status_view) {

    // Class providing query functions for applications and resources
    sys = &system;

    // Initialization
    auto result = Init();
    if (result != SCHED_OK) {
        logger->Fatal("Schedule: initialization failed");
        return SCHED_ERROR;
    } else {
        logger->Debug("Schedule: resource status view = %ld",
            sched_status_view);	
    }

    /** 
    * INSERT YOUR CODE HERE
    **/
    auto assign_awm = std::bind(
        static_cast<ExitCode_t (Adaptive_cpuSchedPol::*)(ba::AppCPtr_t)>
        (&Adaptive_cpuSchedPol::AssignWorkingMode),
        this, _1);
    
    ForEachApplicationToScheduleDo(assign_awm);
    if (result != SCHED_OK)
        return result;
    
    logger->Debug("Schedule: done");
    // Return the new resource status view according to the new resource
    // allocation performed
    status_view = sched_status_view;

    return SCHED_DONE;
}

} // namespace plugins

} // namespace bbque
