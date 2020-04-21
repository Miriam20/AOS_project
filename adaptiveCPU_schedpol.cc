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

#include "adaptiveCPU_schedpol.h"

#include <cstdlib>
#include <cstdint>
#include <iostream>
#include <stdio.h>
#include <fstream>
#include "bbque/modules_factory.h"
#include "bbque/utils/logging/logger.h"

#include "bbque/app/working_mode.h"
#include "bbque/res/binder.h"

#include "bbque/binding_manager.h"

#define MODULE_CONFIG SCHEDULER_POLICY_CONFIG "." SCHEDULER_POLICY_NAME

#define INITIAL_DEFAULT_QUOTA 150
#define MIN_ASSIGNABLE_QUOTA 10
#define ADMISSIBLE_DELTA 10
#define THRESHOLD 1

using namespace std::placeholders;

namespace bu = bbque::utils;
namespace po = boost::program_options;

namespace bbque { 
    namespace plugins {
    
// :::::::::::::::::::::: Static plugin interface ::::::::::::::::::::::::::::

void * AdaptiveCPUSchedPol::Create(PF_ObjectParams *) {
    return new AdaptiveCPUSchedPol();
    }

    int32_t AdaptiveCPUSchedPol::Destroy(void * plugin) {
    if (!plugin)
        return -1;
    delete (AdaptiveCPUSchedPol *)plugin;
    return 0;
    }

    // ::::::::::::::::::::: Scheduler policy module interface :::::::::::::::::::

    char const * AdaptiveCPUSchedPol::Name() {
    return SCHEDULER_POLICY_NAME;
    }

    AdaptiveCPUSchedPol::AdaptiveCPUSchedPol():
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


    AdaptiveCPUSchedPol::~AdaptiveCPUSchedPol() {

}

SchedulerPolicyIF::ExitCode_t AdaptiveCPUSchedPol::_Init() {
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
    nr_not_run_apps = sys->SchedulablesCount(ba::Schedulable::READY);
    nr_not_run_apps += sys->SchedulablesCount(ba::Schedulable::THAWED);
    nr_not_run_apps += sys->SchedulablesCount(ba::Schedulable::RESTORING);
    
    nr_run_apps = sys->SchedulablesCount(ba::Schedulable::RUNNING);
    
    nr_apps = nr_not_run_apps + nr_run_apps;
    logger->Info("Init: nr. active applications = %d", nr_apps);
    
    available_cpu = ra.Available("sys.cpu.pe");
    
    po::options_description opts_desc("AdaptiveCPUSchedPol Parameters Options");
    opts_desc.add_options()
        ("AdaptiveCPUSchedPol.neg_delta",
        po::value<int64_t>(
        &this->neg_delta)->default_value(DEFAULT_NEG_DELTA),
        "Value of neg_delta");

    opts_desc.add_options()
        ("AdaptiveCPUSchedPol.kp",
        po::value<float>(
        &this->kp)->default_value(DEFAULT_KP),
        "Value of coefficient kp");

    opts_desc.add_options()
        ("AdaptiveCPUSchedPol.ki",
        po::value<float>(
        &this->ki)->default_value(DEFAULT_KI),
        "Value of coefficient ki");

    opts_desc.add_options()
        ("AdaptiveCPUSchedPol.kd",
        po::value<float>(
        &this->kd)->default_value(DEFAULT_KD),
        "Value of coefficient kd");
    po::variables_map opts_vm;
    cm.ParseConfigurationFile(opts_desc, opts_vm);

    logger->Info("Running with neg_delta=%d, kp=%f, ki=%f, kd=%f", 
                 neg_delta, kp, ki, kd);

    return SCHED_OK;
}




/********************************
************ MY CODE*************
********************************/

void AdaptiveCPUSchedPol::ComputeQuota(AppInfo_t * ainfo){
    logger->Info("Computing quota for [%s]", ainfo->papp->StrId());
    
    if (!ainfo->papp->Running()){
    
        logger->Info("Computing quota first round");

        if (quota_not_run_apps < INITIAL_DEFAULT_QUOTA)
            ainfo->next_quota = quota_not_run_apps;
        else           
            ainfo->next_quota = INITIAL_DEFAULT_QUOTA;
        
        ainfo->pawm = std::make_shared<ba::WorkingMode>(
        ainfo->papp->WorkingModes().size(), "Default", 1, ainfo->papp);
                 
        //Set initial integral error
        ainfo->papp->SetAttribute("ierr",std::to_string(0));
        
        //Set initial error for derivative controller
        ainfo->papp->SetAttribute("derr",std::to_string(0)); 
        
        available_cpu -= ainfo->next_quota;
            
        logger->Info("Next quota=%d, Previous quota=%d, Previously used CPU=%d, Delta=%d Available cpu=%d",
            ainfo->next_quota,
            ainfo->prev_quota,
            ainfo->prev_used,
            ainfo->prev_delta,
            available_cpu);
        
        return;
    }
    
    //if cpu_usage==quota we push a forfait delta
    if (ainfo->prev_used >= ainfo->prev_quota - THRESHOLD){
        ainfo->prev_delta = neg_delta;
    }
    
    int64_t error, ierr, derr;
    int64_t cv, pvar, ivar, dvar;
    
    //PROPORTIONAL CONTROLLER:
    error = ADMISSIBLE_DELTA/2 - ainfo->prev_delta;
    
    //we are in the admissible range
    if (abs(error) < ADMISSIBLE_DELTA/2)
        error = 0;
    
    pvar = kp*error;

    //INTEGRAL CONTROLLER
    ierr = std::stoll(ainfo->papp->GetAttribute("ierr")) + error;
    ivar = ki*ierr;
    
    //DERIVATIVE CONTROLLER
    derr = error - std::stoll(ainfo->papp->GetAttribute("derr"));
    dvar = kd*derr;
    
    logger->Info("pvar=%d, ivar=%d, dvar=%d", pvar, ivar, dvar);
    //Compute control variable
    cv = pvar + ivar + dvar;
    
    //check available cpu
    if (cv > 0)
       cv = (available_cpu > cv) ? cv : available_cpu;
    else
        //reset in case of fault
        if (abs(cv) > ainfo->prev_quota){
            logger->Error("App [%s] requires quota lower than zero: resetting to initial default quota", ainfo->papp->StrId());
            ainfo->next_quota = (available_cpu > INITIAL_DEFAULT_QUOTA) ? INITIAL_DEFAULT_QUOTA : available_cpu;
        }
    
    ainfo->next_quota = ainfo->prev_quota + cv;
    
    //Create AWM
    ainfo->pawm = std::make_shared<ba::WorkingMode>(
        ainfo->papp->WorkingModes().size(), "Adaptation", 1, ainfo->papp);
    
    //Update errors and expected delta
    ainfo->papp->SetAttribute("ierr",std::to_string(ierr));
	ainfo->papp->SetAttribute("derr",std::to_string(error)); 
        
    //update available cpu
    if (ainfo->next_quota > ainfo->prev_quota)
        available_cpu -= ainfo->next_quota - ainfo->prev_quota;
    else
        available_cpu += ainfo->prev_quota - ainfo->next_quota;
    
    logger->Info("Error = %d, cv=%d",
                 error,
                 cv);
    
    logger->Info("New settings: Next quota=%d, Previous quota=%d, Previously used CPU=%d, Delta=%d Available cpu=%d",
            ainfo->next_quota,
            ainfo->prev_quota,
            ainfo->prev_used,
            ainfo->prev_delta,
            available_cpu);
    
}

AppInfo_t AdaptiveCPUSchedPol::InitializeAppInfo(bbque::app::AppCPtr_t papp){
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
    
    return ainfo;
}
    

SchedulerPolicyIF::ExitCode_t
AdaptiveCPUSchedPol::AssignWorkingMode(bbque::app::AppCPtr_t papp)
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
    
    //it initializes a struct with all the info about the app, to have it in a compact way
    AppInfo_t ainfo = AdaptiveCPUSchedPol::InitializeAppInfo(papp);
    
    logger->Info("Initialized app [%s] info: Previous quota=%d, Previously used CPU=%d, Delta=%d Available cpu=%d",
            papp->StrId(),
            ainfo.prev_quota,
            ainfo.prev_used,
            ainfo.prev_delta,
            available_cpu);
    
    if (quota_not_run_apps == 0 && !ainfo.papp->Running()){
        logger->Info("AssignWorkingMode: Not enough available resources to schedule [%s]",
            papp->StrId());
        return SCHED_SKIP_APP;
    }
        
    AdaptiveCPUSchedPol::ComputeQuota(&ainfo);
    
    auto pawm = ainfo.pawm;
    
    pawm->AddResourceRequest(
        "sys.cpu.pe",
        ainfo.next_quota,
        br::ResourceAssignment::Policy::SEQUENTIAL);
    
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

SchedulerPolicyIF::ExitCode_t 
AdaptiveCPUSchedPol::ScheduleApplications(std::function
<SchedulerPolicyIF::ExitCode_t(bbque::app::AppCPtr_t) > do_func)
{
            
    AppsUidMapIt app_it;
    ba::AppCPtr_t app_ptr;

    //Fair alternative among not running applications
    app_ptr = sys->GetFirstRunning(app_it);
    for (; app_ptr; app_ptr = sys->GetNextRunning(app_it)) {
            do_func(app_ptr);
    }
    
    if (nr_not_run_apps != 0)
        quota_not_run_apps = available_cpu / nr_not_run_apps;
    
    app_ptr = sys->GetFirstReady(app_it);
    for (; app_ptr; app_ptr = sys->GetNextReady(app_it)) {
            do_func(app_ptr);
    }

    app_ptr = sys->GetFirstThawed(app_it);
    for (; app_ptr; app_ptr = sys->GetNextThawed(app_it)) {
            do_func(app_ptr);
    }

    app_ptr = sys->GetFirstRestoring(app_it);
    for (; app_ptr; app_ptr = sys->GetNextRestoring(app_it)) {
            do_func(app_ptr);
    }
    
    return SCHED_OK;
    
    //Other alternative: assign quota>0 only to the number of apps s.t. quota_not_run_apps>=MIN_ASSIGNABLE_QUOTA
    //comment above and uncomment here if preferred
    /* AppsUidMapIt app_it;
    ba::AppCPtr_t app_ptr;
    
    app_ptr = sys->GetFirstRunning(app_it);
    for (; app_ptr; app_ptr = sys->GetNextRunning(app_it)) {
            do_func(app_ptr);
    }
    
    if (nr_not_run_apps != 0)
        quota_not_run_apps = available_cpu / nr_not_run_apps;
        
    uint64_t nr_schedulable_app;
    nr_schedulable_app = nr_not_run_apps;
    
    if ( available_cpu >= MIN_ASSIGNABLE_QUOTA && quota_not_run_apps < MIN_ASSIGNABLE_QUOTA)
        while (quota_not_run_apps >= MIN_ASSIGNABLE_QUOTA){
            nr_schedulable_app--;
            quota_not_run_apps = available_cpu / nr_schedulable_app;
        }
    
    app_ptr = sys->GetFirstReady(app_it);
    for (; app_ptr; app_ptr = sys->GetNextReady(app_it)) {
        if (nr_schedulable_app == 0)
            quota_not_run_apps = 0;
        do_func(app_ptr);
        nr_schedulable_app--;
    }

    app_ptr = sys->GetFirstThawed(app_it);
    for (; app_ptr; app_ptr = sys->GetNextThawed(app_it)) {
        if (nr_schedulable_app == 0)
            quota_not_run_apps = 0;
        do_func(app_ptr);
        nr_schedulable_app--;
    }

    app_ptr = sys->GetFirstRestoring(app_it);
    for (; app_ptr; app_ptr = sys->GetNextRestoring(app_it)) {
        if (nr_schedulable_app == 0)
            quota_not_run_apps = 0;
        do_func(app_ptr);
        nr_schedulable_app--;
    }

    return SCHED_OK;*/
}

/********************************
************ MY CODE*************
********************************/


SchedulerPolicyIF::ExitCode_t
AdaptiveCPUSchedPol::Schedule(
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
    
    
    /*auto assign_awm = std::bind(
        static_cast<ExitCode_t (AdaptiveCPUSchedPol::*)(ba::AppCPtr_t)>
        (&AdaptiveCPUSchedPol::AssignWorkingMode),
        this, _1);
    
    result = ForEachApplicationToScheduleDo(assign_awm);
    if (result != SCHED_OK)
        return result;
    */
    
    auto assign_awm = std::bind(
        static_cast<ExitCode_t (AdaptiveCPUSchedPol::*)(ba::AppCPtr_t)>
        (&AdaptiveCPUSchedPol::AssignWorkingMode),
        this, _1);
    
    result = AdaptiveCPUSchedPol::ScheduleApplications(assign_awm);
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
