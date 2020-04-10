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
#define SPAZIO 10
using namespace std::placeholders;

namespace bu = bbque::utils;
namespace po = boost::program_options;

namespace bbque { 
    namespace plugins {
    
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

void Adaptive_cpuSchedPol::ComputeQuota(AppInfo_t * ainfo){
    logger->Info("Computing quota for [%s]", ainfo->papp->StrId());
    
    if (ainfo->pawm == nullptr){
    
        logger->Info("Computing quota first round");

        if (available_cpu < INITIAL_DEFAULT_QUOTA)
            ainfo->next_quota = available_cpu;
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
    
    /*TEMPORARY: To elude cpu_usage problem
     * I assume that the app needs more resources
     * */
    if ( ainfo->prev_delta < 0){
        ainfo->prev_delta = 0;
    }
    
    int64_t error, ierr, derr;
    int64_t var, pvar, ivar, dvar;
    
    //PROPORTIONAL CONTROLLER:
    //Less quota required
    if (ainfo->prev_delta > ADMISSIBLE_DELTA)
	    error = ainfo->prev_delta - ADMISSIBLE_DELTA/2;
    
    //More quota required
    else if (ainfo->prev_delta == 0)
	    error = SPAZIO;
    
	//We converged into an admissible delta    
    else {
        ainfo->next_quota = ainfo->prev_quota;
        
        ainfo->pawm = std::make_shared<ba::WorkingMode>(
        ainfo->papp->WorkingModes().size(), "Convergent", 1, ainfo->papp);
    
        logger->Info("New settings: Next quota=%d, Previous quota=%d, Previously used CPU=%d, Delta=%u Available cpu=%d",
                ainfo->next_quota,
                ainfo->prev_quota,
                ainfo->prev_used,
                ainfo->prev_delta,
                available_cpu);
        return;
    }
    pvar = kp*error;

    //INTEGRAL CONTROLLER
    ierr = std::stoll(ainfo->papp->GetAttribute("ierr")) + error;
    ivar = ki*ierr;
    
    //DERIVATIVE CONTROLLER
    derr = error - std::stoll(ainfo->papp->GetAttribute("derr"));
    dvar = kd*derr;
    
    //Compute cumulative variation
    var = pvar + ivar + dvar;
    
    //check available cpu
    if (var > 0)
        var = (available_cpu > var) ? var : available_cpu;
    
    ainfo->next_quota = ainfo->prev_quota + var;
    
    //Create WM
    ainfo->pawm = std::make_shared<ba::WorkingMode>(
        ainfo->papp->WorkingModes().size(), "Adaptation", 1, ainfo->papp);
    
    //Update errors
    ainfo->papp->SetAttribute("ierr",std::to_string(ierr));
	ainfo->papp->SetAttribute("derr",std::to_string(error)); 
    
    //update available cpu
    if (ainfo->next_quota > ainfo->prev_quota)
        available_cpu -= ainfo->next_quota - ainfo->prev_quota;
    else
        available_cpu += ainfo->prev_quota - ainfo->next_quota;
    
    logger->Info("New settings: Next quota=%d, Previous quota=%d, Previously used CPU=%d, Delta=%u Available cpu=%d",
            ainfo->next_quota,
            ainfo->prev_quota,
            ainfo->prev_used,
            ainfo->prev_delta,
            available_cpu);
    
    
}
/*void Adaptive_cpuSchedPol::ComputeQuota(AppInfo_t * ainfo)
{
    logger->Info("Computing quota for [%s]", ainfo->papp->StrId());
    
    if (ainfo->pawm == nullptr){
    
        logger->Info("Computing quota first round");

        if (available_cpu < INITIAL_DEFAULT_QUOTA)
            ainfo->next_quota = available_cpu;
        else           
            ainfo->next_quota = INITIAL_DEFAULT_QUOTA;
        
        ainfo->pawm = std::make_shared<ba::WorkingMode>(
        ainfo->papp->WorkingModes().size(), "Default", 1, ainfo->papp);
    	
	//set initial integral positive and negative error
	ainfo->papp->SetAttribute("iperr",std::to_string(0));
	ainfo->papp->SetAttribute("inerr",std::to_string(0));
	
	//set initial derivative positive and negative error
	ainfo->papp->SetAttribute("dperr",std::to_string(0)); 
	ainfo->papp->SetAttribute("dnerr",std::to_string(0)); 
        
	available_cpu -= ainfo->next_quota;
        
        logger->Info("Next quota=%d, Previous quota=%d, Previously used CPU=%d, Delta=%d Available cpu=%d",
            ainfo->next_quota,
            ainfo->prev_quota,
            ainfo->prev_used,
            ainfo->prev_delta,
            available_cpu);
        
        return;
    }
    */
    /*TEMPORARY: To elude cpu_usage problem
     * I assume that the app needs more resources
     * */
   /* if ( ainfo->prev_delta > ainfo->prev_quota){
        ainfo->prev_delta = 0;
    }
    
    uint64_t iperr, inerr;
    SNum_t err, ierr, derr;
    SNum_t pvar, ivar, dvar, var;

    //proportional controller
    if (ainfo->prev_delta > ADMISSIBLE_DELTA){
	    err.value = ainfo->prev_delta - ADMISSIBLE_DELTA/2;
	    err.sign = NEG;
    }
    else if (ainfo->prev_delta == 0){
	    err.value = SPAZIO;
	    err.sign = POS;
    }
    
    //Error=0, we converged 
    else{ 
        
        ainfo->next_quota = ainfo->prev_quota;
        
        ainfo->pawm = std::make_shared<ba::WorkingMode>(
        ainfo->papp->WorkingModes().size(), "Convergent", 1, ainfo->papp);
    
        logger->Info("New settings: Next quota=%d, Previous quota=%d, Previously used CPU=%d, Delta=%u Available cpu=%d",
                ainfo->next_quota,
                ainfo->prev_quota,
                ainfo->prev_used,
                ainfo->prev_delta,
                available_cpu);
        return;
    }

    pvar.value = err.value*kp;
    pvar.sign = err.sign;

    //integral controller
    iperr = std::stoull(ainfo->papp->GetAttribute("iperr"));
    inerr = std::stoull(ainfo->papp->GetAttribute("inerr"));

    if (err.sign == POS) iperr += err.value;
    else inerr += err.value;

    if (iperr > inerr) {
	    ierr.value = iperr - inerr;
	    ierr.sign = POS;
    }
    else{
	    ierr.value = inerr - iperr;
	    ierr.sign = NEG;
    }
    
    ivar.value = ierr.value*ki;
    ivar.sign = ierr.sign;


    //derivative controller
    if (stoull(ainfo->papp->GetAttribute("dperr"))){
        logger->Info("interno if");
	    derr.value = stoull(ainfo->papp->GetAttribute("dperr"));
	    derr.value = POS;
    }
    else{
        derr.value = stoull(ainfo->papp->GetAttribute("dnerr"));
        derr.value = NEG;
    }
    
    dvar = ComputeSub(err, derr);
    dvar.value *= kd;
    
    var = ComputeSum(pvar, ivar, dvar);
    
    if (var.sign == POS){
        if (available_cpu < var.value)
            var.value = available_cpu;
        
        ainfo->next_quota = ainfo->prev_quota + var.value;
    }

    else
        ainfo->next_quota = ainfo->prev_quota + var.value;
    
    //update errors    
    if (err.sign == POS){
        ainfo->papp->SetAttribute("dnerr", std::to_string(0));
        ainfo->papp->SetAttribute("dperr", std::to_string(err.value));
        ainfo->papp->SetAttribute("iperr", std::to_string(iperr));
    }
    else{
        ainfo->papp->SetAttribute("dnerr", std::to_string(err.value));
        ainfo->papp->SetAttribute("dperr", std::to_string(0));
        ainfo->papp->SetAttribute("inerr",std::to_string(inerr));
    }
    
    ainfo->pawm = std::make_shared<ba::WorkingMode>(
        ainfo->papp->WorkingModes().size(), "Adaptation", 1, ainfo->papp);
    
    if (ainfo->next_quota > ainfo->prev_quota)
        available_cpu -= ainfo->next_quota - ainfo->prev_quota;
    else
        available_cpu += ainfo->prev_quota - ainfo->next_quota;
    
    logger->Info("New settings: Next quota=%d, Previous quota=%d, Previously used CPU=%d, Delta=%u Available cpu=%d",
            ainfo->next_quota,
            ainfo->prev_quota,
            ainfo->prev_used,
            ainfo->prev_delta,
            available_cpu);
}*/


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
    
    //it just initializes a struct with all the info about the app, to have it in a compact way
    AppInfo_t ainfo = Adaptive_cpuSchedPol::InitializeAppInfo(papp);
    
    logger->Info("Initialized app [%s] info: Previous quota=%d, Previously used CPU=%d, Delta=%u Available cpu=%d",
            papp->StrId(),
            ainfo.prev_quota,
            ainfo.prev_used,
            ainfo.prev_delta,
            available_cpu);
    
    if (available_cpu == 0 && ainfo.prev_delta == 0){
        if (ainfo.pawm == nullptr){
            logger->Info("AssignWorkingMode: Not enough available resources to schedule [%s]",
                papp->StrId());
            return SCHED_SKIP_APP;
        }
        else {
            logger->Error("AssignWorkingMode: Not enough available resources to increase quota of [%s]",
                papp->StrId());
            am.ScheduleRequestAsPrev(papp, sched_status_view);
            return SCHED_OK;
        }
    }
        
    Adaptive_cpuSchedPol::ComputeQuota(&ainfo);
    
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
    kp = 0.2;
    ki = 0.2;
    kd = 0.2;
    
    available_cpu = ra.Available("sys.cpu.pe");
    
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
