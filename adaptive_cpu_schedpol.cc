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

#define MODULE_CONFIG SCHEDULER_POLICY_CONFIG "." SCHEDULER_POLICY_NAME

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

	return SCHED_OK;
}




/********************************
************ MY CODE*************
********************************/

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
	// Build a new working mode featuring assigned resources
	ba::AwmPtr_t pawm = papp->CurrentAWM();
	if (pawm == nullptr) {
		pawm = std::make_shared<ba::WorkingMode>(
			papp->WorkingModes().size(), "MEDIOCRE", 1, papp);
	}
    
    // Resource request addition
	pawm->AddResourceRequest(
		"sys.cpu.pe",
		100,
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