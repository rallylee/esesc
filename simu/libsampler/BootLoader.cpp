// Contributed by Jose Renau
//
// The ESESC/BSD License
//
// Copyright (c) 2005-2013, Regents of the University of California and 
// the ESESC Project.
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are met:
//
//   - Redistributions of source code must retain the above copyright notice,
//   this list of conditions and the following disclaimer.
//
//   - Redistributions in binary form must reproduce the above copyright
//   notice, this list of conditions and the following disclaimer in the
//   documentation and/or other materials provided with the distribution.
//
//   - Neither the name of the University of California, Santa Cruz nor the
//   names of its contributors may be used to endorse or promote products
//   derived from this software without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
// AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
// IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
// ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
// LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
// CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
// SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
// INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
// CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
// ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
// POSSIBILITY OF SUCH DAMAGE.

#include <signal.h>

#include "BootLoader.h"

#ifndef ENABLE_NOEMU
#include "QEMUEmulInterface.h"
#endif
#ifdef ENABLE_CUDA
#include "GPUEmulInterface.h"
#endif

#include "SamplerSMARTS.h"
#include "SamplerPeriodic.h"

#ifdef ENABLE_CUDA
#include "SamplerGPUSim.h"
#include "SamplerGPUSpacial.h"
#endif

#include "GProcessor.h"
#include "OoOProcessor.h"
#include "InOrderProcessor.h"
#include "GPUSMProcessor.h"
#include "GMemorySystem.h"
#include "../libmem/MemorySystem.h"

#include "Report.h"
#include "SescConf.h"
#include "DrawArch.h"

extern DrawArch arch;

extern "C" void signalCatcher(int32_t sig);

extern "C" void signalCatcherUSR1(int32_t sig) {

  MSG("WARNING: signal %d received. Dumping partial statistics\n", sig);

  BootLoader::reportOnTheFly();

  signal(SIGUSR1,signalCatcherUSR1);
}

extern "C" void signalCatcher(int32_t sig) {

  MSG("Stopping simulation early");

  static bool sigFaulting=false;
  if( sigFaulting ) {
    TaskHandler::unplug();
    MSG("WARNING. Not a nice stop. It may leave pids");
    kill(-getpid(),SIGKILL);
    abort();
  }
  sigFaulting=true;

  MSG("WARNING: unexpected signal %d received. Dumping partial statistics\n", sig);
  signal(SIGUSR1,signalCatcher); // Even sigusr1 should go here

  BootLoader::reportOnTheFly();

  BootLoader::unboot();
  BootLoader::unplug();

  sigFaulting=false;

  abort();
}

char       *BootLoader::reportFile;
timeval     BootLoader::stTime;
PowerModel *BootLoader::pwrmodel;
bool        BootLoader::doPower;

void BootLoader::check() 
{
  if (!SescConf->check()) {
    Report::field("**** ESESC CONFIGURATION INCORRECT ****");
    fprintf(stderr, "\n\n**** ESESC CONFIGURATION INCORRECT ****\n\nDeleting report File %s\n\n",Report::getNameID());
    remove(Report::getNameID());
    exit(-1);
  }
}

void BootLoader::reportOnTheFly(const char *file) {
  char *tmp;

  if( !file )
    file = reportFile;

  tmp = (char *)malloc(strlen(file)+1);
  strcpy(tmp,file);

  Report::openFile(tmp);

  SescConf->dump();

  report("partial");

  free(tmp);
}

void BootLoader::startReportOnTheFly() {
  char *tmp;

  const char * file = reportFile;

  tmp = (char *)malloc(strlen(file)+1);
  strcpy(tmp,file);

  Report::openFile(tmp);

  SescConf->dump();
  pwrmodel->startDump();
  report("partial");

  free(tmp);
}

void BootLoader::stopReportOnTheFly() {
  pwrmodel->stopDump();
}

void BootLoader::report(const char *str) {

  timeval endTime;
  gettimeofday(&endTime, 0);
  Report::field("OSSim:reportName=%s", str);
  Report::field("OSSim:beginTime=%s", ctime(&stTime.tv_sec));
  Report::field("OSSim:endTime=%s", ctime(&endTime.tv_sec));

  double msecs = (endTime.tv_sec - stTime.tv_sec) * 1000
    + (endTime.tv_usec - stTime.tv_usec) / 1000;

  TaskHandler::report(str);

  Report::field("OSSim:msecs=%8.2f" ,(double)msecs / 1000);

  GStats::report(str);

  Report::close();
}

void BootLoader::plugEmulInterfaces() {

  FlowID nemul = SescConf->getRecordSize("","cpuemul");

  // nemul will give me the total number of flows (cpuemuls)  in the system. 
  // out of these, some can be CPU, some can be GPU. (interleaved as well) 
  I(nemul>0);
  
  LOG("I: cpuemul size [%d]",nemul);

  // For now, we will assume the simplistic case where there is one QEMU and one GPU. 
  // (So one object each of classes QEMUEmulInterface and GPUEmulInterface)
  const char* QEMUCPUSection = NULL;
#ifdef ENABLE_CUDA
  const char* QEMUGPUSection = NULL;
  FlowID sharedGPUFid=0;
#endif
  for(FlowID i=0;i<nemul;i++) {
    const char *section = SescConf->getCharPtr("","cpuemul",i);
    const char *type    = SescConf->getCharPtr(section,"type");

    if(strcasecmp(type,"QEMU") == 0 ) {
      if (QEMUCPUSection == NULL) {
        QEMUCPUSection = section;
      } else if (strcasecmp(QEMUCPUSection,section)){
        MSG("ERROR: eSESC supports only a single instance of QEMU");
        MSG("cpuemul[%d] specifies a different section %s",i,section);
        SescConf->notCorrect();
        return;
      }
      TaskHandler::FlowIDEmulMapping.push_back(0); // Interface 0 is QEMU

      createEmulInterface(QEMUCPUSection, i); // each CPU has it's own Emul/Sampler

#ifdef ENABLE_CUDA
    }else if(strcasecmp(type,"GPU") == 0 ) {
      if (QEMUGPUSection == NULL) {
        QEMUGPUSection = section;
        sharedGPUFid = i;
        createEmulInterface(QEMUGPUSection, sharedGPUFid); // GPU shares one Emul/Sampler
      } else if (strcasecmp(QEMUGPUSection,section)){
        MSG("ERROR: eSESC supports only a single GPU");
        MSG("cpuemul[%d] specifies a different section %s",i,section);
        SescConf->notCorrect();
        return;
      }
      TaskHandler::FlowIDEmulMapping.push_back(1); // Interface 1 is GPU
#endif
    }else{
      MSG("ERROR: Unknown type %s of section %s, cpuemul [%d]",type,section,i);
      SescConf->notCorrect();
      return;
    }
  }
}

void BootLoader::plugSimuInterfaces() {

  FlowID nsimu = SescConf->getRecordSize("","cpusimu");

  LOG("I: cpusimu size [%d]",nsimu);

  for(FlowID i=0;i<nsimu;i++) {
    const char *section = SescConf->getCharPtr("","cpusimu",i);
    createSimuInterface(section, i);
  }
}

EmuSampler *BootLoader::getSampler(const char *section, const char *keyword, EmulInterface *eint, FlowID fid) 
{
  const char *sampler_sec  = SescConf->getCharPtr(section,keyword);
  const char *sampler_type = SescConf->getCharPtr(sampler_sec,"type");

  EmuSampler *sampler = 0;
  if(strcasecmp(sampler_type,"inst") == 0 ) {
    sampler = new SamplerSMARTS("TASS",sampler_sec,eint, fid);
  }else if(strcasecmp(sampler_type,"time") == 0 ) {
    sampler = new SamplerPeriodic("TBS",sampler_sec,eint, fid);
#ifdef ENABLE_CUDA
  }else if(strcasecmp(sampler_type,"GPUSpacial") == 0 ) {
    I(strcasecmp(sampler_type,"GPUSpacial")==0);
    sampler = new SamplerGPUSpacial("GPUSpacial",sampler_sec,eint, fid);
#endif
  }else{
    MSG("ERROR: unknown sampler [%s] type [%s]",sampler_sec,sampler_type);
    SescConf->notCorrect();
  }

  return sampler;
}

void BootLoader::createEmulInterface(const char *section, FlowID fid) 
{
#ifndef ENABLE_NOEMU
  const char *type    = SescConf->getCharPtr(section,"type");
  
  if (type==0) {
    MSG("ERROR: type field should be defined in section [%s]",section);
    SescConf->notCorrect();
    return;
  }

  EmulInterface *eint = 0;
  if(strcasecmp(type,"QEMU") == 0 ) {
    eint = new QEMUEmulInterface(section);
    TaskHandler::addEmul(eint, fid);
#ifdef ENABLE_CUDA
  }else if(strcasecmp(type,"GPU") == 0 ) {
    eint = new GPUEmulInterface(section);
    TaskHandler::addEmulShared(eint);
#endif
  }else{
    MSG("ERROR: unknown cpusim [%s] type [%s]",section,type);
    SescConf->notCorrect();
    return;
  }
  I(eint);


  EmuSampler *sampler = getSampler(section,"sampler",eint, fid);
  I(sampler);
  eint->setSampler(sampler, fid);
#endif
}

void BootLoader::createSimuInterface(const char *section, FlowID i) {

  GMemorySystem *gms = 0;
  if(SescConf->checkInt(section,"noMemory")) {
    gms = new DummyMemorySystem(i);
  }else{
    gms = new MemorySystem(i);
  }
  gms->buildMemorySystem();

  CPU_t cpuid = static_cast<CPU_t>(i);

  GProcessor  *gproc = 0;
  if(SescConf->getBool("cpusimu","inorder",cpuid)) {
    gproc =new InOrderProcessor(gms, cpuid);
#ifdef ENABLE_CUDA
  } else if(SescConf->checkInt(section,"sp_per_sm")) {
    if( SescConf->getInt(section,"sp_per_sm") >= 1 ){
      MSG("SPs per SM in the GPU = %d", SescConf->getInt(section,"sp_per_sm"));
      gproc = new GPUSMProcessor(gms, cpuid);
    } else {
      MSG("Invalid number of sp_per_sm. Exiting...");
      SescConf->notCorrect();
    }
#endif
  } else {
    gproc =new OoOProcessor(gms, cpuid);
  }

  I(gproc);
  TaskHandler::addSimu(gproc);
}

void BootLoader::plug(int argc, const char **argv) {
  // Before boot
  SescConf = new SConfig(argc, argv);

  pwrmodel = new PowerModel;

  TaskHandler::plugBegin();
  plugSimuInterfaces();
  check();
  
  const char *tmp;
  if( getenv("REPORTFILE") ) {
    tmp = strdup(getenv("REPORTFILE"));
  }else{
    tmp = SescConf->getCharPtr("", "reportFile",0);
  }
  if( getenv("REPORTFILE2") ) {
    const char *tmp2 = strdup(getenv("REPORTFILE2"));
    reportFile = (char *)malloc(30 + strlen(tmp)+strlen(tmp2));
    sprintf(reportFile,"esesc_%s_%s.XXXXXX",tmp,tmp2);
  }else{
    reportFile = (char *)malloc(30 + strlen(tmp));
    sprintf(reportFile,"esesc_%s.XXXXXX",tmp);
  }

  Report::openFile(reportFile);
  
  SescConf->getDouble("technology","frequency"); // Just read it to get it in the dump

  check();
  plugEmulInterfaces();
  check();


  const char *pwrsection = SescConf->getCharPtr("","pwrmodel",0);
  doPower = SescConf->getBool(pwrsection,"doPower",0);
  if(doPower) {
    pwrmodel->plug(pwrsection);
    check();
  }else{
    MSG("Power calculations disabled");
  }

  check();
  TaskHandler::plugEnd();
#if GOOGLE_GRAPH_API
  arch.drawArchHtml("memory-arch.html");
#else
  arch.drawArchDot("memory-arch.dot");
#endif
}

void BootLoader::boot() {
  gettimeofday(&stTime, 0);

  if (!SescConf->lock())
    exit(-1);

  SescConf->dump();

  TaskHandler::boot();
}

void BootLoader::unboot() {

  MSG("BootLoader::unboot called... Finishing the work");

#ifdef ESESC_THERM
  ReportTherm::stopCB();
#endif

  TaskHandler::unboot();
}

void BootLoader::unplug() {
  // after unboot

#ifdef ESESC_THERM
  ReportTherm::stopCB();
  ReportTherm::close();
#endif

  TaskHandler::unplug();
#ifdef ESESC_POWERMODEL
  if (doPower)
    pwrmodel->unplug();
#endif
}

