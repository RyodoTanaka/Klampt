#include "ODESimulator.h"
#include "ODECommon.h"
#include "ODECustomGeometry.h"
#include "Settings.h"
#include <list>
#include <fstream>
//#include "Geometry/Clusterize.h"
#include <KrisLibrary/geometry/ConvexHull2D.h>
#include <KrisLibrary/statistics/KMeans.h>
#include <KrisLibrary/statistics/HierarchicalClustering.h>
#include <KrisLibrary/utils/EquivalenceMap.h>
#include <KrisLibrary/utils/permutation.h>
#include <ode/ode.h>
#include <KrisLibrary/Timer.h>
#include <KrisLibrary/myfile.h>
#ifndef WIN32
#include <unistd.h>
#endif //WIN32

#define TEST_READ_WRITE_STATE 0
#define DO_TIMING 0

const static size_t gMaxKMeansSize = 5000;
const static size_t gMaxHClusterSize = 2000;
static size_t gPreclusterContacts = 0;
static double gClusterTime = 0;
static double gContactDetectTime = 0;

//if at the beginning of the timestep, the two objects are touching with depth d in the boundary layer
//of size m, but after the timestep, they are penetrating the boundary layer, the sim will roll back
//until the new depth d' gives a remaining margin of (m-d') >= c*(m-d) where c<1 is this fraction.
const static double gRollbackPenetrationFraction = 0.5;  

//stuff for contact detection callbacks
struct ODEContactResult
{
  dGeomID o1,o2;
  vector<dContactGeom> contacts;
  vector<dJointFeedback> feedback;
  bool meshOverlap;
};

const static int max_contacts = 1000;
static dContactGeom gContactTemp[max_contacts];
static list<ODEContactResult> gContacts;
static vector<ODEContactResult*> gContactsVector;


//Method for identifying objects via dGeomSetData/dGeomGetData
//max objects: 500 million =(
const static int terrainMarker = 0x80000000;
const static int rigidObjectMarker = 0x40000000;
const static int robotMarker = 0x20000000;
const static int markerMask = 0xf0000000;
const static int robotIndexMask = 0x0fff0000;
const static int robotIndexShift = 16;
const static int robotLinkIndexMask = 0x0000ffff;
const static int robotLinkIndexShift = 0;
void* TerrainIndexToGeomData(int terrain) { return (void*)(terrainMarker | terrain); }
void* ObjectIndexToGeomData(int object) { return (void*)(rigidObjectMarker | object); }
void* RobotIndexToGeomData(int robot,int link) { assert(robot <= 0xfff && link <= 0xffff); return (void*)(robotMarker | (robot << robotIndexShift) | link); }
int GeomDataToTerrainIndex(void* p) { intptr_t d = (intptr_t)p; if(!(d & terrainMarker)) return -1; return int(d & ~terrainMarker); }
int GeomDataToObjectIndex(void* p) { intptr_t d = (intptr_t)p; if(!(d & rigidObjectMarker)) return -1; return int(d & ~rigidObjectMarker); }
int GeomDataToRobotIndex(void* p) { intptr_t d = (intptr_t)p; if(!(d & robotMarker)) return -1; return int((d & ~robotMarker) >> robotIndexShift); }
int GeomDataToRobotLinkIndex(void* p) { intptr_t d = (intptr_t)p; if(!(d & robotMarker)) return -1; return int((d & ~robotMarker) & robotLinkIndexMask); }
void* ObjectIDToGeomData(const ODEObjectID& id)
{
  if(id.IsEnv()) return TerrainIndexToGeomData(id.index);
  if(id.IsRigidObject()) return ObjectIndexToGeomData(id.index);
  if(id.IsRobot()) return RobotIndexToGeomData(id.index,id.bodyIndex);
  return NULL;
}
ODEObjectID GeomDataToObjectID(void* p)
{
  intptr_t d = (intptr_t)p;
  if(d & terrainMarker) return ODEObjectID(0,GeomDataToTerrainIndex(p));
  if(d & rigidObjectMarker) return ODEObjectID(2,GeomDataToObjectIndex(p));
  if(d & robotMarker) return ODEObjectID(1,GeomDataToRobotIndex(p),GeomDataToRobotLinkIndex(p));
  FatalError("Invalid ODE geom data pointer %p",p);
  return ODEObjectID();
}

template <class T>
bool TestReadWriteState(T& obj,const char* name="")
{
  File fwrite,fwritenew;
  fwrite.OpenData();
  if(!obj.WriteState(fwrite)) {
    fprintf(stderr,"WriteState %s failed\n",name);
    return false;
  }
  //HACK for File internal buffer length bug returning buffer capacity rather
  //than size
  //int n = fwrite.Length();
  int n1 = fwrite.Position();
  fwrite.Seek(0,FILESEEKSTART);
  if(!obj.ReadState(fwrite)) {
    fprintf(stderr,"ReadState %s failed\n",name);
    return false;
  }
  fwritenew.OpenData();
  if(!obj.WriteState(fwritenew)) {
    fprintf(stderr,"Second WriteState %s failed\n",name);
    return false;
  }
  //HACK for File internal buffer length bug returning buffer capacity rather
  //than size
  //int n2 = fwritenew.Length();
  int n2 = fwritenew.Position();
  char* d1 = (char*)fwrite.GetDataBuffer();
  char* d2 = (char*)fwritenew.GetDataBuffer();
  if(n1 != n2) {
    fprintf(stderr,"WriteState %s wrote different numbers of bytes: %d -> %d\n",name,n1,n2);
    return false;
  }
  for(int i=0;i<n1;i++) {
    if(d1[i] != d2[i]) {
      fprintf(stderr,"WriteState %s wrote different byte at position %d/%d: 0x%x vs 0x%x\n",name,i,n1,(int)d1[i],(int)d2[i]);
      return false;
    }
  }
  return true;
}


//contact merging -- these are now handled by the contact clustering procedure
const static bool kMergeContacts = false;
const static Real kContactPosMergeTolerance = 3e-3;
const static Real kContactOriMergeTolerance = 1e-1;


ODESimulatorSettings::ODESimulatorSettings()
{
  gravity[0] = gravity[1] = 0;
  gravity[2] = -9.8;
  defaultEnvPadding = gDefaultEnvPadding;
  defaultEnvSurface.kFriction = 0.3;
  defaultEnvSurface.kRestitution = 0.1;
  defaultEnvSurface.kStiffness = 800000;
  defaultEnvSurface.kDamping = 200000;
  //defaultEnvSurface.kStiffness = Inf;
  //defaultEnvSurface.kDamping = Inf;

  boundaryLayerCollisions = gBoundaryLayerCollisionsEnabled;
  rigidObjectCollisions = gRigidObjectCollisionsEnabled;
  robotSelfCollisions = gRobotSelfCollisionsEnabled;
  robotRobotCollisions = gRobotRobotCollisionsEnabled;

  adaptiveTimeStepping = gAdaptiveTimeStepping;

  maxContacts = 20;
  clusterNormalScale = 0.1;

  errorReductionParameter = 0.95;
  dampedLeastSquaresParameter = 1e-6;
}

inline Real ERPFromSpring(Real timestep,Real kP,Real kD)
{
  return timestep*kP/(timestep*kP+kD);
}

inline Real CFMFromSpring(Real timestep,Real kP,Real kD)
{
  return 1.0/(timestep*kP+kD);
}



struct ODEObject 
{
  bool gODEInitialized;
  ODEObject () : gODEInitialized(false) {}
  void Init() {
    if(!gODEInitialized) {
      #ifdef dDOUBLE
      if(dCheckConfiguration("ODE_double_precision")!=1) {
	FatalError("ODE is compiled with single precision but Klamp't is compiled with double, either reconfigure ODE with --enable-double-precision or recompile Klamp't with dDOUBLE");
      }
      #else
      if(dCheckConfiguration("ODE_single_precision")!=1) {
	FatalError("ODE is compiled with double precision but Klamp't is compiled with single, either reconfigure ODE without --enable-double-precision or recompile Klamp't with dSINGLE");
      }
      #endif

      printf("Initializing ODE...\n");
      dInitODE();
      InitODECustomGeometry();
      gODEInitialized = true;
    }
  }
  ~ODEObject() { 
    if(gODEInitialized) {
      printf("Closing ODE...\n");
      dCloseODE(); 
    }
  }
};

ODEObject g_ODE_object;





ODESimulator::ODESimulator()
{
  simTime = 0;
  timestep = 0;
  lastStateTimestep = 0;

  g_ODE_object.Init();
  worldID = dWorldCreate();
  contactGroupID = dJointGroupCreate(0);
  //envSpaceID = dHashSpaceCreate(0);
  //dHashSpaceSetLevels(envSpaceID,-2,1);
  envSpaceID = dSimpleSpaceCreate(0);

  dWorldSetERP(worldID,settings.errorReductionParameter);
  dWorldSetCFM(worldID,settings.dampedLeastSquaresParameter);
  dWorldSetGravity(worldID,settings.gravity[0],settings.gravity[1],settings.gravity[2]);
}

void ODESimulator::SetGravity(const Vector3& g)
{
  g.get(settings.gravity);
  dWorldSetGravity(worldID,g.x,g.y,g.z);
}

void ODESimulator::SetERP(double erp)
{
  settings.errorReductionParameter = erp;
  dWorldSetERP(worldID,erp);
}

void ODESimulator::SetCFM(double cfm)
{
  settings.dampedLeastSquaresParameter = cfm;
  dWorldSetCFM(worldID,cfm);
}

ODESimulator::~ODESimulator()
{
  dJointGroupDestroy(contactGroupID);
  for(size_t i=0;i<terrainGeoms.size();i++)
    delete terrainGeoms[i];
  for(size_t i=0;i<robots.size();i++)
    delete robots[i];
  dSpaceDestroy(envSpaceID);
  dWorldDestroy(worldID);
}

void ODESimulator::AddTerrain(Terrain& terr)
{
  terrains.push_back(&terr);
  terrainGeoms.resize(terrainGeoms.size()+1);
  terrainGeoms.back() = new ODEGeometry;
  terrainGeoms.back()->Create(&*terr.geometry,envSpaceID,Vector3(Zero),settings.boundaryLayerCollisions);
  terrainGeoms.back()->surf() = settings.defaultEnvSurface;
  terrainGeoms.back()->SetPadding(settings.defaultEnvPadding);
  if(!terr.kFriction.empty())
    terrainGeoms.back()->surf().kFriction = terr.kFriction[0];
  //set the geom data pointer
  dGeomSetData(terrainGeoms.back()->geom(),TerrainIndexToGeomData((int)terrains.size()-1));
  //printf("Terrain %d GeomData set to %p\n",terrains.size()-1,TerrainIndexToGeomData((int)terrains.size()-1));
  dGeomSetCategoryBits(terrainGeoms.back()->geom(),0x1);
  dGeomSetCollideBits(terrainGeoms.back()->geom(),0xffffffff ^ 0x1);
}

void ODESimulator::AddRobot(Robot& robot)
{
  robots.push_back(new ODERobot(robot));
  //For some reason, self collisions don't work with hash spaces
  robots.back()->Create(robots.size()-1,worldID,settings.boundaryLayerCollisions);
  //robotStances.resize(robots.size());
  for(size_t i=0;i<robot.links.size();i++)
    if(robots.back()->triMesh(i) && robots.back()->geom(i)) {
      if(robots.back()->robot.parents[i] == -1) { //treat as part of the terrain
	dGeomSetCategoryBits(robots.back()->geom(i),0x1);
	dGeomSetCollideBits(robots.back()->geom(i),0xffffffff ^ 0x1);
      }
      else {
	dGeomSetCategoryBits(robots.back()->geom(i),0x4);
	dGeomSetCollideBits(robots.back()->geom(i),0xffffffff);
      }
    }
}
void ODESimulator::DeleteRobot(const char* s)
{
  for(size_t i=0;i<robots.size();i++)
    if(strcmp(s,robots.at(i)->robot.name.c_str())==0){
      std::cout << "deleting robot " << robots.at(i)->robot.name << std::endl;
      robots.erase(robots.begin() + i);
      break;
    }
}

void ODESimulator::AddObject(RigidObject& object)
{
  objects.push_back(new ODERigidObject(object));
  objects.back()->Create(worldID,envSpaceID,settings.boundaryLayerCollisions);
  dGeomSetData(objects.back()->geom(),ObjectIndexToGeomData(objects.size()-1));
  //printf("Rigid object %d GeomData set to %p\n",objects.size()-1,ObjectIndexToGeomData((int)objects.size()-1));
  dGeomSetCategoryBits(objects.back()->geom(),0x2);
  dGeomSetCollideBits(objects.back()->geom(),0xffffffff);
}

string ODESimulator::ObjectName(const ODEObjectID& obj) const
{
  if(obj.IsEnv()) return terrains[obj.index]->name.c_str();
  if(obj.IsRigidObject()) return objects[obj.index]->obj.name;
  if(obj.IsRobot()) return robots[obj.index]->robot.LinkName(obj.bodyIndex);
  return "invalid object";
}

dBodyID ODESimulator::ObjectBody(const ODEObjectID& obj) const
{
  if(obj.IsEnv()) return NULL;
  if(obj.IsRigidObject()) return objects[obj.index]->body();
  if(obj.IsRobot()) return robots[obj.index]->body(obj.bodyIndex);
  return NULL;
}

dGeomID ODESimulator::ObjectGeom(const ODEObjectID& obj) const
{
  if(obj.IsEnv()) return terrainGeoms[obj.index]->geom();
  if(obj.IsRigidObject()) return objects[obj.index]->geom();
  if(obj.IsRobot()) return robots[obj.index]->geom(obj.bodyIndex);
  return NULL;
}

typedef pair<ODEObjectID,ODEObjectID> CollisionPair;

//call this after DetectCollisions() to update the collision margin state, determine a list of objects to be concerned about
//for adaptive time stepping.
void GetCurrentCollisionStatus(ODESimulator* sim,
    map<CollisionPair,double>& marginsRemaining,
    vector<CollisionPair >& concernedObjects)
{
  marginsRemaining.clear();
  concernedObjects.resize(0);
  for(list<ODEContactResult>::iterator i=gContacts.begin();i!=gContacts.end();i++) {
    CollisionPair collpair(GeomDataToObjectID(dGeomGetData(i->o1)),GeomDataToObjectID(dGeomGetData(i->o2)));
    if(collpair.second < collpair.first) 
      swap(collpair.first,collpair.second);

    //if two bodies had overlap on the prior timestep, don't
    //keep rolling back
    if(i->meshOverlap) { 
      if(sim->lastMarginsRemaining.count(collpair) == 0 || sim->lastMarginsRemaining[collpair] > 0) {
        concernedObjects.push_back(collpair);
      }
      marginsRemaining[collpair] = 0;
    }
    else {
      //no overlap, still we should consider rolling back if the remaining margin drops significantly 
      //get the closest pair of points
      CustomGeometryData* g1 = dGetCustomGeometryData(i->o1);
      CustomGeometryData* g2 = dGetCustomGeometryData(i->o2);
      double margin = g1->outerMargin + g2->outerMargin;
      double depth = 0;
      string id1=sim->ObjectName(collpair.first),id2=sim->ObjectName(collpair.second);
      for(size_t j=0;j<i->contacts.size();j++)
        depth = Max(depth,(double)i->contacts[j].depth);
      //printf("ODESimulation: normal penetration depth between bodies %s and %s is %g/%g\n",id1.c_str(),id2.c_str(),depth,margin);
      double oldmargin = (sim->lastMarginsRemaining.count(collpair) == 0 ? margin : sim->lastMarginsRemaining[collpair]);
      if((margin - depth) < gRollbackPenetrationFraction*oldmargin) {
        //if there was some previous collision margin, we should do a rollback if the margins were reduced even further
        concernedObjects.push_back(collpair); 
      }
      marginsRemaining[collpair] = margin-depth;
    }
  }
}

void PrintStatus(ODESimulator* sim,const CollisionPair& collpair,const char* predescription="Concerned objects",const char* postdescription="have")
{
  printf("  %s %s - %s %s position ",predescription,sim->ObjectName(collpair.first).c_str(),sim->ObjectName(collpair.second).c_str(),postdescription);

  const dReal* p1 = sim->ObjectBody(collpair.first) ? dBodyGetPosition(sim->ObjectBody(collpair.first)) : NULL;
  const dReal* p2 = sim->ObjectBody(collpair.second) ? dBodyGetPosition(sim->ObjectBody(collpair.second)) : NULL;
  const dReal* v1 = sim->ObjectBody(collpair.first) ? dBodyGetLinearVel(sim->ObjectBody(collpair.first)) : NULL;
  const dReal* v2 = sim->ObjectBody(collpair.second) ? dBodyGetLinearVel(sim->ObjectBody(collpair.second)) : NULL;
  const dReal* w1 = sim->ObjectBody(collpair.first) ? dBodyGetAngularVel(sim->ObjectBody(collpair.first)) : NULL;
  const dReal* w2 = sim->ObjectBody(collpair.second) ? dBodyGetAngularVel(sim->ObjectBody(collpair.second)) : NULL;
  if(p1)
    printf("%g %g %g and ",p1[0],p1[1],p1[2]);
  else 
    printf("NONE and ");
  if(p2)
    printf("%g %g %g\n",p2[0],p2[1],p2[2]);
  else
    printf("NONE\n");
  printf("  velocity ");
  if(p1)
    printf("%g %g %g and ",v1[0],v1[1],v1[2]);
  else 
    printf("NONE and ");
  if(p2)
    printf("%g %g %g\n",v2[0],v2[1],v2[2]);
  else
    printf("NONE\n");
  printf("  angular vel ");
  if(p1)
    printf("%g %g %g and ",w1[0],w1[1],w1[2]);
  else 
    printf("NONE and ");
  if(p2)
    printf("%g %g %g\n",w2[0],w2[1],w2[2]);
  else
    printf("NONE\n");
}

void PrintStatus(ODESimulator* sim,const vector<CollisionPair >& concernedObjects,const char* predescription="Concerned objects",const char* postdescription="have")
{
  for(size_t i=0;i<concernedObjects.size();i++) {
    ::PrintStatus(sim,concernedObjects[i],predescription,postdescription);
  }
}

void ODESimulator::Step(Real dt)
{
  Assert(timestep == 0);

#if DO_TIMING
  Timer timer;
  double collisionTime,stepTime,updateTime;
  gContactDetectTime = gClusterTime = 0;
  gPreclusterContacts = 0;
#endif // DO_TIMING

  if(settings.adaptiveTimeStepping) {
#if DO_TIMING
    collisionTime = 0;
    stepTime = 0;
#endif

    //normal adaptive time step method:
    //ATS(dt)
    //1. valid_time <- 0, timestep <- dt, desired_time = dt
    //2. last <- save_state()
    //3. while(valid_time < desired_time) 
    //4.   step(timestep)
    //5.   detectcollisions()
    //6.   if(rollback) 
    //7.     load_state(last)
    //8.     timestep *= 0.5
    //9.   else
    //10.    valid_time += timestep
    //11.    timestep = desired_time - valid_time
    //12.    last <- save_state()
    //but to be stateless we need to detect collisions first. 
    //hence, the normal loop is
    //TS(dt)
    //1. detect collisions
    //2. step(dt)
    //for adaptive time stepping, we need a modified loop where we cut the
    //ATS loop right after the first instance of line 4. The first step
    //performs the ATS loop after the first instance of line 4 for the PRIOR
    //step, and then it performs the first instance up to line 4.
    //1. valid_time <- -lastdt, timestep <- lastdt, desired_time = 0
    //2. while(true)
    //3.   detectcollisions()
    //4.   if(rollback) 
    //5.     load_state(last)
    //6.     timestep *= 0.5
    //7.   else
    //8.     valid_time += timestep
    //9.     timestep = desired_time - valid_time
    //10.    last <- save_state()
    //11.  if(valid_time >= desired_time) break
    //11.  step(timestep)
    //12. //last <- save_state() can skip this step since it was saved on
    //    //line 10
    //13. step(dt)
    //14. lastdt = dt
    vector<CollisionPair > concernedObjects;
  	if(lastStateTimestep > 0) {
  		timestep=lastStateTimestep;
  		Real validTime = -lastStateTimestep, desiredTime = 0;
  		bool didRollback = false;
  		while(true) {
  		  DetectCollisions();
  	#if DO_TIMING
  		  collisionTime += timer.ElapsedTime();
  		  timer.Reset();
  	#endif // DO_TIMING
  		  //determine whether to rollback
        bool rollback = false;
        map<CollisionPair,double> marginsRemaining;
        vector<CollisionPair > newConcernedObjects;
        GetCurrentCollisionStatus(this,marginsRemaining,newConcernedObjects);
        rollback = !newConcernedObjects.empty();
        for(size_t i=0;i<newConcernedObjects.size();i++) {
          const CollisionPair& collpair = newConcernedObjects[i];
          if(marginsRemaining[collpair] == 0) {
            if(!didRollback) {
              string id1=ObjectName(collpair.first),id2=ObjectName(collpair.second);
              //printf("ODESimulation: rolling back due to new penetration between bodies %s and %s\n",id1.c_str(),id2.c_str());
              //if(lastMarginsRemaining.count(collpair) == 0)
              //  printf("  no previous contact\n");
              //else
              //  printf("  previously had depth %g\n",lastMarginsRemaining[collpair]);
            }
            //PrintStatus(this,collpair,"Colliding objects","found collision at");
          }
          else {
            if(true || !didRollback) {
              string id1=ObjectName(collpair.first),id2=ObjectName(collpair.second);
              //printf("ODESimulation: rolling back due to increasing penetration between bodies %s and %s\n",id1.c_str(),id2.c_str());
              //if(lastMarginsRemaining.count(collpair) == 0)
              //  printf("  margin shrank from no-contact to %g\n",marginsRemaining[collpair]);
              //else
              //  printf("  margin shrank from %g to %g\n",lastMarginsRemaining[collpair],marginsRemaining[collpair]);
            }
          }
        }
  		  if(rollback && !lastState.IsOpen()) {
          //printf("ODESimulation: Rollback rejected because last state not saved\n");
          getchar();
          rollback = false;
  		  }
  		  if(rollback && timestep < 1e-6) {
  		    //printf("ODESimulation: Rollback rejected because timestep %g below minimum threshold\n",timestep);
          //getchar();

          //TODO: DEBUG THIS PRINTOUT STUFF -- it changes the state of the adaptive time stepper
          /*
          //TEMP: print out remaining configuration
          printf("POST TINY STEP CONFIGURATION:\n");
          PrintStatus(this,concernedObjects,"Concerned objects after step","had");
          for(size_t i=0;i<concernedObjects.size();i++) {
            if(marginsRemaining.count(concernedObjects[i]) == 0)
              printf("%s %s not even close\n",ObjectName(concernedObjects[i].first).c_str(),ObjectName(concernedObjects[i].second).c_str());
            else
              printf("%s %s margin %g\n",ObjectName(concernedObjects[i].first).c_str(),ObjectName(concernedObjects[i].second).c_str(),marginsRemaining[concernedObjects[i]]);
          }

          //TEMP: print out starting configuration (make sure to save current state to temp then load it back)
          File temp;
          bool res = temp.OpenData(FILEREAD | FILEWRITE);
          Assert(res);
          Assert(temp.IsOpen());
          WriteState(temp);
          
          lastState.Seek(0,FILESEEKSTART);
          ReadState(lastState);
          printf("STARTING CONFIGURATION:\n");
          PrintStatus(this,concernedObjects,"Concerned objects originally","had");
          DetectCollisions();
          GetCurrentCollisionStatus(this,marginsRemaining,newConcernedObjects);
          for(size_t i=0;i<concernedObjects.size();i++) {
            if(marginsRemaining.count(concernedObjects[i]) == 0)
              printf("%s %s not even close\n",ObjectName(concernedObjects[i].first).c_str(),ObjectName(concernedObjects[i].second).c_str());
            else
              printf("%s %s margin %g\n",ObjectName(concernedObjects[i].first).c_str(),ObjectName(concernedObjects[i].second).c_str(),marginsRemaining[concernedObjects[i]]);
          }

          temp.Seek(0,FILESEEKSTART);
          ReadState(temp);
          DetectCollisions();
          GetCurrentCollisionStatus(this,marginsRemaining,concernedObjects);

          printf("DOUBLE CHECKING THATPOST TINY STEP CONFIGURATION RESTORED:\n");
          PrintStatus(this,concernedObjects,"Concerned objects after step","had");
          for(size_t i=0;i<concernedObjects.size();i++) {
            if(marginsRemaining.count(concernedObjects[i]) == 0)
              printf("%s %s not even close\n",ObjectName(concernedObjects[i].first).c_str(),ObjectName(concernedObjects[i].second).c_str());
            else
              printf("%s %s margin %g\n",ObjectName(concernedObjects[i].first).c_str(),ObjectName(concernedObjects[i].second).c_str(),marginsRemaining[concernedObjects[i]]);
          }
          printf("Press enter to continue...\n");
          getchar();
          */

          rollback = false;
  		  }
  	
  		  if(rollback) {
          //printf("ODESimulation: Rolling back at time %g, time step halved to %g\n",simTime,timestep*0.5);
          //PrintStatus(this,concernedObjects,"Backing up colliding objects","from");
          
          didRollback = true;
          lastState.Seek(0,FILESEEKSTART);
          ReadState(lastState);
          timestep *= 0.5;

          //PrintStatus(this,concernedObjects,"Backed up colliding objects","to previous");
          concernedObjects = newConcernedObjects;
          Assert(concernedObjects.size() > 0);

          //TEMP: verify collision status wasn't changed by reading last state?
          DetectCollisions();
          GetCurrentCollisionStatus(this,marginsRemaining,newConcernedObjects);
          /*
          if(marginsRemaining != lastMarginsRemaining) {
            printf("ODESimulation: Warning, difference between rolled-back re-detected margins and previous margins?\n");
            printf("Press enter to continue\n");
            getchar();
          }
          */
  		  }
  		  else {
          //accept prior step
          lastState.Close();
          bool res = lastState.OpenData(FILEREAD | FILEWRITE);
          Assert(res);
          Assert(lastState.IsOpen());
          WriteState(lastState);
          for(size_t i=0;i<concernedObjects.size();i++) {
            if(marginsRemaining.count(concernedObjects[i]) == 0) {
              //printf("ODESimulation: collision %s - %s erased entirely\n",ObjectName(concernedObjects[i].first).c_str(),ObjectName(concernedObjects[i].second).c_str());
            }
            else {
              double d=marginsRemaining[concernedObjects[i]];
              //if(lastMarginsRemaining.count(concernedObjects[i])) 
                //printf("ODESimulation: collision %s - %s changed from no contact to depth %g\n",ObjectName(concernedObjects[i].first).c_str(),ObjectName(concernedObjects[i].second).c_str(),d);
              //else
                //printf("ODESimulation: collision %s - %s changed from depth %g to depth %g\n",ObjectName(concernedObjects[i].first).c_str(),ObjectName(concernedObjects[i].second).c_str(),lastMarginsRemaining[concernedObjects[i]],d);
            }
          }
          if(didRollback)
            //printf("ODESimulation: Adaptive sub-step of size %g is valid, arriving at time %g.\n",timestep,simTime);
          //if(didRollback) {
          //  PrintStatus(this,concernedObjects,"Colliding objects","now at");
          //}
          concernedObjects.resize(0);
          swap(lastMarginsRemaining,marginsRemaining);
       
          validTime += timestep;
          simTime += timestep;
          timestep = desiredTime-validTime;
          //if(didRollback)
            //printf("   reset time step to %g.\n",timestep);
          didRollback = false;
  		  }
  		  if(validTime >= desiredTime) break;

        //heres where we make the tentative step, to be checked for collisions at the start of the next loop
        //if(didRollback) printf("Trying step of size %g\n",timestep);
        //NOW set up the contact response for the previous timestep
        SetupContactResponse();
        //PrintStatus(this,concernedObjects,"Colliding objects","pre-step");
  		  StepDynamics(timestep);
        //PrintStatus(this,concernedObjects,"Colliding objects","post-step");
  		}
  		if(didRollback) {
  		  //printf("ODESimulation: Adaptive time step done, arrived at time %g.\n",simTime);
  		}
  	}
  	else {
  		//first step
  		timestep=dt;
  		DetectCollisions();
  	#if DO_TIMING
  		collisionTime += timer.ElapsedTime();
  		timer.Reset();
  	#endif // DO_TIMING
  		//determine whether to rollback
  		bool rollback = false;
  		map<CollisionPair,double> marginsRemaining;
  		for(list<ODEContactResult>::iterator i=gContacts.begin();i!=gContacts.end();i++) {
  		  CollisionPair collpair(GeomDataToObjectID(dGeomGetData(i->o1)),GeomDataToObjectID(dGeomGetData(i->o2)));
  		  if(i->meshOverlap) { 
  		    rollback = true;
  		    marginsRemaining[collpair] = 0;
  		  }
        else {
          //no overlap, still we should consider rolling back if the remaining margin drops significantly 
          //get the closest pair of points
          CustomGeometryData* g1 = dGetCustomGeometryData(i->o1);
          CustomGeometryData* g2 = dGetCustomGeometryData(i->o2);
          double margin = g1->outerMargin + g2->outerMargin;
          double depth = 0;
          for(size_t j=0;j<i->contacts.size();j++)
            depth = Max(depth,(double)i->contacts[j].depth);
          marginsRemaining[collpair] = margin - depth;
        }
  		}
  		if(rollback) {
  			//printf("ODESimulation: Warning, initial state has underlying meshes overlapping\n");
  			for(map<CollisionPair,double>::const_iterator i=marginsRemaining.begin();i!=marginsRemaining.end();i++) {
          if(i->second <= 0) {
    			  CollisionPair collpair = i->first;
            string id1=ObjectName(collpair.first),id2=ObjectName(collpair.second);
            //printf("  %s - %s\n",id1.c_str(),id2.c_str());
          }
  			}
  			printf("Press enter to continue...\n");
  			getchar();
  			//NO ROLLBACK ON FIRST
  			rollback = false;
  		}
      
  		//save state
  		lastState.Close();
  		bool res = lastState.OpenData(FILEREAD | FILEWRITE);
  		Assert(lastState.IsOpen());
  		WriteState(lastState);
  		lastMarginsRemaining = marginsRemaining;
  	}
    //do the prospective time step for the next call
    timestep = dt;
    SetupContactResponse();
    lastStateTimestep = dt;
    StepDynamics(dt);
  }
  else {
    //plain old constant time-stepping

    gContacts.clear();
    gContactsVector.resize(0);
    
    timestep=dt;
    DetectCollisions();
    SetupContactResponse();

  //printf("  %d contacts detected\n",gContacts.size());

#if DO_TIMING
    collisionTime = timer.ElapsedTime();
    timer.Reset();
#endif // DO_TIMING

    StepDynamics(dt);
    simTime += dt;

#if DO_TIMING
    stepTime = timer.ElapsedTime();
    timer.Reset();
#endif // DO_TIMING
  }

  //copy out feedback forces
  for(map<CollisionPair,ODEContactList>::iterator i=contactList.begin();i!=contactList.end();i++) {  
    ODEContactList& cl=i->second;
    cl.forces.clear();
    cl.penetrating = false;
    for(size_t j=0;j<cl.feedbackIndices.size();j++) {
      int k=cl.feedbackIndices[j];
      Assert(k >= 0 && k < (int)gContactsVector.size());
      ODEContactResult* cres = gContactsVector[k];
      if(cres->meshOverlap) cl.penetrating = true;
      Vector3 temp;
      for(size_t i=0;i<cres->feedback.size();i++) {
	CopyVector(temp,cres->feedback[i].f1);
	cl.forces.push_back(temp);
	/*
	if(!cl.points.back().isValidForce(-temp)) {
	  printf("ODESimulator: Warning, solved contact force %d %d is invalid\n",k,i);
	  cout<<"  Force: "<<-temp<<", normal "<<cl.points.back().n<<" kFriction "<<cl.points.back().kFriction<<endl;
	}
	*/
      }
    }
    if(!cl.feedbackIndices.empty())
      Assert(cl.points.size() == cl.forces.size());
  }

  timestep = 0;

#if DO_TIMING
  updateTime = timer.ElapsedTime();
#endif //DO_TIMING

#if DO_TIMING
  ofstream out("odesimulator_timing.csv",ios::app);
  if(out.tellp()==0) {
    cout<<"Saving to odesimulator_timing.csv"<<endl;
    out<<"total,#preclusterContacts,#contacts,collision detection,contact detect,clustering,dynamics step,misc update"<<endl;
  }
  size_t nc = 0;
  for(list<ODEContactResult>::iterator i=gContacts.begin();i!=gContacts.end();i++)
    nc += i->contacts.size();
  out<<collisionTime+stepTime+updateTime<<","<<gPreclusterContacts<<","<<nc<<","<<collisionTime<<","<<gContactDetectTime<<","<<gClusterTime<<","<<stepTime<<","<<updateTime<<endl;
#endif

  //KH: commented this out so GetContacts() would work for ContactSensor simulation.  Be careful about loading state
  //gContacts.clear();
}


struct EqualPlane
{
  //position tolerance and angle tolerance, in radians
  EqualPlane(double xtol,double atol) : ptol(xtol),catol(Cos(atol)) {}

  //assumes normalized normals
  bool operator () (const dContactGeom& a,const dContactGeom& b)
  {
    Vector3 an,bn;
    CopyVector(an,a.normal);
    CopyVector(bn,b.normal);
    Real ndot = an.dot(bn);
    if(ndot > catol) {
      Vector3 ax,bx;
      CopyVector(ax,a.pos);
      CopyVector(bx,b.pos);
      return  FuzzyEquals(ax.dot(an),bx.dot(an),ptol) && 
	FuzzyEquals(ax.dot(bn),bx.dot(bn),ptol);
    }
    return false;
  }

  double ptol,catol;
};

struct EqualPoint
{
  //position tolerance and angle tolerance, in radians
  EqualPoint(double xtol) : ptol(xtol) {}

  //assumes normalized normals
  bool operator () (const dContactGeom& a,const dContactGeom& b)
  {
    Vector3 ax,bx;
    CopyVector(ax,a.pos);
    CopyVector(bx,b.pos);
    return  ax.distanceSquared(bx) < Sqr(ptol);
  }

  double ptol;
};


/** Two ways of merging frictionless contacts without resorting to 6D
 * wrench space
 * 1) interior points in 2D convex hull, with the projection defined 
 *    by the contact normal
 * 2) interior points in convex cone at a single contact point (TODO)
 * 
 * If contacts have friction, the convex hull method only works when the
 * points are on the same plane.
 */
void CHContactsPlane(vector<dContactGeom>& contacts)
{
  if(contacts.empty()) return;
  if(contacts.size() <= 2) return;

  Vector3 c(Zero),n(Zero);
  for(size_t i=0;i<contacts.size();i++) {
    Vector3 cx,nx;
    CopyVector(cx,contacts[i].pos);
    CopyVector(nx,contacts[i].normal);
    c += cx;
    n += nx;
  }
  c /= contacts.size();
  n.inplaceNormalize();

  //get the deepest contact
  size_t deepest = 0;
  for(size_t i=1;i<contacts.size();i++) 
    if(contacts[i].depth > contacts[deepest].depth) deepest=i;

  //make a plane
  Vector3 x,y;
  n.getOrthogonalBasis(x,y);
  //Real nofs = n.dot(c);
  Real xofs = x.dot(c);
  Real yofs = y.dot(c);
  vector<Vector2> pt2d(contacts.size());
  for(size_t i=0;i<contacts.size();i++) {
    Vector3 cx;
    CopyVector(cx,contacts[i].pos);
    pt2d[i].x = x.dot(cx)-xofs;
    pt2d[i].y = y.dot(cx)-yofs;
  }

  //get the convex hull
  vector<Vector2> ch(contacts.size());
  vector<int> mapping(contacts.size());
  int num=Geometry::ConvexHull2D_Chain_Unsorted(&pt2d[0],contacts.size(),&ch[0],&mapping[0]);
  vector<dContactGeom> temp(num);
  bool addDeepest = true;
  for(int i=0;i<num;i++) {
    Assert(mapping[i] < (int)contacts.size());
    temp[i] = contacts[mapping[i]];
    if(mapping[i] == (int)deepest)
      addDeepest = false;
  }
  if(addDeepest)
    temp.push_back(contacts[deepest]);
  swap(temp,contacts);
}

void interpolate(const dContactGeom& a,const dContactGeom& b,Real u,dContactGeom& x)
{
  for(int i=0;i<3;i++) {
    x.pos[i] = (1-u)*a.pos[i]+u*b.pos[i];
  }
  Vector3 an,bn;
  CopyVector(an,a.normal);
  CopyVector(bn,b.normal);
  Vector3 n = (1-u)*an+u*bn;
  n.inplaceNormalize();
  CopyVector(x.normal,n);

  x.depth = Max(a.depth,b.depth);
  x.g1 = a.g1;
  x.g2 = a.g2;
  Assert(x.g1 == b.g1);
  Assert(x.g2 == b.g2);
}


void ClusterContactsMerge(vector<dContactGeom>& contacts,int maxClusters,Real clusterNormalScale)
{
  if((int)contacts.size() <= maxClusters) return;
  vector<Vector> pts(contacts.size());
  for(size_t i=0;i<pts.size();i++) {
    pts[i].resize(7);
    pts[i][0] = contacts[i].pos[0];
    pts[i][1] = contacts[i].pos[1];
    pts[i][2] = contacts[i].pos[2];
    pts[i][3] = contacts[i].normal[0]*clusterNormalScale;
    pts[i][4] = contacts[i].normal[1]*clusterNormalScale;
    pts[i][5] = contacts[i].normal[2]*clusterNormalScale;
    pts[i][6] = contacts[i].depth;
  }

  //Timer timer;
  Statistics::HierarchicalClustering clust;
  clust.Build(pts,maxClusters,Statistics::HierarchicalClustering::AverageLinkage);
  //cout<<"Clustering time: "<<timer.ElapsedTime()<<endl;
  /*
  cout<<"Points"<<endl;
  for(size_t i=0;i<pts.size();i++)
    cout<<pts[i]<<endl;
  */

  //read out the clusters
  contacts.resize(maxClusters);
  for(int i=0;i<maxClusters;i++) {
    const vector<int>& inds = clust.Cluster(i);
    /*
    cout<<"Cluster "<<i<<": ";
    for(size_t j=0;j<inds.size();j++)
      cout<<inds[j]<<", ";
    cout<<endl;
    */
    Vector mean(7,Zero);
    for(size_t j=0;j<inds.size();j++) 
      mean += pts[inds[j]];
    mean /= inds.size();

    contacts[i].pos[0] = mean[0];
    contacts[i].pos[1] = mean[1];
    contacts[i].pos[2] = mean[2];
    contacts[i].normal[0] = mean[3]/clusterNormalScale;
    contacts[i].normal[1] = mean[4]/clusterNormalScale;
    contacts[i].normal[2] = mean[5]/clusterNormalScale;

    Real len = Vector3(contacts[i].normal[0],contacts[i].normal[1],contacts[i].normal[2]).length();
    if(FuzzyZero(len) || !IsFinite(len)) {
      printf("ODESimulator: Warning, clustered normal became zero/infinite\n");
      int found = inds[0];
      contacts[i].pos[0] = pts[found][0];
      contacts[i].pos[1] = pts[found][1];
      contacts[i].pos[2] = pts[found][2];
      contacts[i].normal[0] = pts[found][3];
      contacts[i].normal[1] = pts[found][4];
      contacts[i].normal[2] = pts[found][5];
      Real len = Vector3(contacts[i].normal[0],contacts[i].normal[1],contacts[i].normal[2]).length();
      contacts[i].normal[0] /= len;
      contacts[i].normal[1] /= len;
      contacts[i].normal[2] /= len;
      contacts[i].depth = pts[found][6];
      continue;
    }
    contacts[i].normal[0] /= len;
    contacts[i].normal[1] /= len;
    contacts[i].normal[2] /= len;
    contacts[i].depth = mean[6];
  }
}

void ClusterContactsKMeans(vector<dContactGeom>& contacts,int maxClusters,Real clusterNormalScale)
{
  if((int)contacts.size() <= maxClusters) return;
  vector<Vector> pts(contacts.size());
  for(size_t i=0;i<pts.size();i++) {
    pts[i].resize(7);
    pts[i][0] = contacts[i].pos[0];
    pts[i][1] = contacts[i].pos[1];
    pts[i][2] = contacts[i].pos[2];
    pts[i][3] = contacts[i].normal[0]*clusterNormalScale;
    pts[i][4] = contacts[i].normal[1]*clusterNormalScale;
    pts[i][5] = contacts[i].normal[2]*clusterNormalScale;
    pts[i][6] = contacts[i].depth;
  }

  Statistics::KMeans kmeans(pts,maxClusters);
  //randomized
  //kmeans.RandomInitialCenters();
  //deterministic
  for(size_t i=0;i<kmeans.centers.size();i++)
    kmeans.centers[i] = kmeans.data[(i*pts.size())/kmeans.centers.size()];
  int iters=20;
  kmeans.Iterate(iters);
  contacts.resize(kmeans.centers.size());
  vector<int> degenerate;
  for(size_t i=0;i<contacts.size();i++) {
    contacts[i].pos[0] = kmeans.centers[i][0];
    contacts[i].pos[1] = kmeans.centers[i][1];
    contacts[i].pos[2] = kmeans.centers[i][2];
    contacts[i].normal[0] = kmeans.centers[i][3]/clusterNormalScale;
    contacts[i].normal[1] = kmeans.centers[i][4]/clusterNormalScale;
    contacts[i].normal[2] = kmeans.centers[i][5]/clusterNormalScale;
    contacts[i].depth = kmeans.centers[i][6];
    Real len = Vector3(contacts[i].normal[0],contacts[i].normal[1],contacts[i].normal[2]).length();
    if(FuzzyZero(len) || !IsFinite(len)) {
      printf("ODESimulator: Warning, clustered normal became zero/infinite\n");
      //pick any in the cluster
      int found = -1;
      for(size_t k=0;k<kmeans.labels.size();k++) {
	if(kmeans.labels[k] == (int)i) {
	  found = (int)k;
	  break;
	}
      }
      if(found < 0) {
	//strange -- degenerate cluster?
	degenerate.push_back(i);
	continue;
      }
      contacts[i].pos[0] = pts[found][0];
      contacts[i].pos[1] = pts[found][1];
      contacts[i].pos[2] = pts[found][2];
      contacts[i].normal[0] = pts[found][3];
      contacts[i].normal[1] = pts[found][4];
      contacts[i].normal[2] = pts[found][5];
      Real len = Vector3(contacts[i].normal[0],contacts[i].normal[1],contacts[i].normal[2]).length();
      contacts[i].normal[0] /= len;
      contacts[i].normal[1] /= len;
      contacts[i].normal[2] /= len;
      contacts[i].depth = pts[found][6];
      continue;
    }
    contacts[i].normal[0] /= len;
    contacts[i].normal[1] /= len;
    contacts[i].normal[2] /= len;
    //cout<<"Clustered contact "<<contacts[i].pos[0]<<" "<<contacts[i].pos[1]<<" "<<contacts[i].pos[2]<<endl;
    //cout<<"Clustered normal "<<contacts[i].normal[0]<<" "<<contacts[i].normal[1]<<" "<<contacts[i].normal[2]<<endl;
    contacts[i].depth = kmeans.centers[i][6];
  }
  reverse(degenerate.begin(),degenerate.end());
  for(size_t i=0;i<degenerate.size();i++) {
    contacts.erase(contacts.begin()+degenerate[i]);
  }
}


bool depthGreater(const dContactGeom& a,const dContactGeom& b)
{
  return a.depth > b.depth;
}


void ClusterContacts(vector<dContactGeom>& contacts,int maxClusters,Real clusterNormalScale)
{
  gPreclusterContacts += contacts.size();

  //for really big contact sets, do a subsampling
  if(contacts.size()*maxClusters > gMaxKMeansSize && contacts.size()*contacts.size() > gMaxHClusterSize) {
    int minsize = Max((int)gMaxKMeansSize/maxClusters,(int)Sqrt(Real(gMaxHClusterSize)));
    printf("ClusterContacts: subsampling %d to %d contacts\n",(int)contacts.size(),minsize);
    vector<dContactGeom> subcontacts(minsize);
    //random subsample
    /*
    vector<int> subsample(contacts.size());
    RandomPermutation(subsample);
    subsample.resize(minsize);
    for(size_t i=0;i<subsample.size();i++)
      subcontacts[i] = contacts[subsample[i]];
    */
    //deterministic subsample
    for(int i=0;i<minsize;i++) {
      subcontacts[i] = contacts[(i*minsize)/contacts.size()];
    }
    swap(subcontacts,contacts);
  }
  size_t hclusterSize = contacts.size()*contacts.size();
  size_t kmeansSize = contacts.size()*maxClusters;
  //if(hclusterSize < gMaxHClusterSize)
  //ClusterContactsMerge(contacts,maxClusters,clusterNormalScale);
  //else 
  ClusterContactsKMeans(contacts,maxClusters,clusterNormalScale);
  /*
  //TEST: contact depth sorting
  if(contacts.size() > maxClusters) {
    sort(contacts.begin(),contacts.end(),depthGreater);
    contacts.resize(maxClusters);
  }
  */
}

void MergeContacts(vector<dContactGeom>& contacts,double posTolerance,double oriTolerance)
{
  EqualPlane eq(posTolerance,oriTolerance);
  vector<vector<int> > sets;
  EquivalenceMap(contacts,sets,eq);

  vector<dContactGeom> res;
  vector<dContactGeom> temp;
  for(size_t i=0;i<sets.size();i++) {
    if(sets[i].empty()) continue;
    temp.resize(sets[i].size());
    for(size_t j=0;j<sets[i].size();j++)
      temp[j] = contacts[sets[i][j]];
    CHContactsPlane(temp);
    //printf("CH Reduced from %d to %d\n",sets[i].size(),temp.size());

    //EqualPoint eq(posTolerance);
    //Clusterize(temp,eq);

    res.insert(res.end(),temp.begin(),temp.end());
  }
  swap(contacts,res);
}

void collisionCallback(void *data, dGeomID o1, dGeomID o2)
{
  Assert(!dGeomIsSpace(o1) && !dGeomIsSpace(o2));

  dBodyID b1 = dGeomGetBody(o1);
  dBodyID b2 = dGeomGetBody(o2);

  // take care of disabled bodies
  if( b1 && !b2 && !dBodyIsEnabled(b1) )
   return; // b1 is disabled and collides with no-body
  if( b2 && !b1 && !dBodyIsEnabled(b2) )
    return; // b2 is disabled and collides with no-body
  if( b1 && b2 && !dBodyIsEnabled(b1) && !dBodyIsEnabled(b2) )
   return; // both b1 and b2 are disabled
  
  ClearCustomGeometryCollisionReliableFlag();
  int num = dCollide (o1,o2,max_contacts,gContactTemp,sizeof(dContactGeom));
  vector<dContactGeom> vcontact(num);
  int numOk = 0;
  for(int i=0;i<num;i++) {
    if(gContactTemp[i].g1 == o2 && gContactTemp[i].g2 == o1) {
      printf("Swapping contact\n");
      std::swap(gContactTemp[i].g1,gContactTemp[i].g2);
      for(int k=0;k<3;k++) gContactTemp[i].normal[k]*=-1.0;
      std::swap(gContactTemp[i].side1,gContactTemp[i].side2);
    }
    Assert(gContactTemp[i].g1 == o1);
    Assert(gContactTemp[i].g2 == o2);
    vcontact[numOk] = gContactTemp[i];
    const dReal* n=vcontact[numOk].normal;
    if(Sqr(n[0])+Sqr(n[1])+Sqr(n[2]) < 0.9 || Sqr(n[0])+Sqr(n[1])+Sqr(n[2]) > 1.2) {
      //GIMPACT will report this
      //printf("Warning, degenerate contact with normal %f %f %f\n",vcontact[numOk].normal[0],vcontact[numOk].normal[1],vcontact[numOk].normal[2]);
      continue;
    }
    numOk++;
  }
  vcontact.resize(numOk);
  
  if(vcontact.size() > 0) {
    gContacts.push_back(ODEContactResult());
    gContacts.back().o1 = o1;
    gContacts.back().o2 = o2;
    swap(gContacts.back().contacts,vcontact);
    gContacts.back().meshOverlap = !GetCustomGeometryCollisionReliableFlag();
  }
  else {
    if(!GetCustomGeometryCollisionReliableFlag()) {
      printf("collision callback: meshes overlapped, but no contacts were generated?\n");
      gContacts.push_back(ODEContactResult());
      gContacts.back().o1 = o1;
      gContacts.back().o2 = o2;
      swap(gContacts.back().contacts,vcontact);
      gContacts.back().meshOverlap = !GetCustomGeometryCollisionReliableFlag();
    }
  }
}

void selfCollisionCallback(void *data, dGeomID o1, dGeomID o2)
{
  ODERobot* robot = reinterpret_cast<ODERobot*>(data);
  Assert(!dGeomIsSpace(o1) && !dGeomIsSpace(o2));
  int link1 = GeomDataToRobotLinkIndex(dGeomGetData(o1));
  int link2 = GeomDataToRobotLinkIndex(dGeomGetData(o2));
  Assert(link1 >= 0 && link1 < (int)robot->robot.links.size());
  Assert(link2 >= 0 && link2 < (int)robot->robot.links.size());
  if(robot->robot.selfCollisions(link1,link2)==NULL) {
    return;
  }
  
  ClearCustomGeometryCollisionReliableFlag();
  int num = dCollide (o1,o2,max_contacts,gContactTemp,sizeof(dContactGeom));
  vector<dContactGeom> vcontact(num);
  int numOk = 0;
  for(int i=0;i<num;i++) {
    if(gContactTemp[i].g1 == o2 && gContactTemp[i].g2 == o1) {
      printf("Swapping contact\n");
      std::swap(gContactTemp[i].g1,gContactTemp[i].g2);
      for(int k=0;k<3;k++) gContactTemp[i].normal[k]*=-1.0;
      std::swap(gContactTemp[i].side1,gContactTemp[i].side2);
    }
    Assert(gContactTemp[i].g1 == o1);
    Assert(gContactTemp[i].g2 == o2);
    vcontact[numOk] = gContactTemp[i];
    const dReal* n=vcontact[numOk].normal;
    if(Sqr(n[0])+Sqr(n[1])+Sqr(n[2]) < 0.9 || Sqr(n[0])+Sqr(n[1])+Sqr(n[2]) > 1.2) {
      printf("Warning, degenerate contact with normal %f %f %f\n",vcontact[numOk].normal[0],vcontact[numOk].normal[1],vcontact[numOk].normal[2]);
      //continue;
    }
    numOk++;
  }
  //TEMP: printing self collisions
  //if(numOk > 0) printf("%d self collision contacts between links %d and %d\n",numOk,(int)link1,(int)link2);
  vcontact.resize(numOk);
  
  if(kMergeContacts && numOk > 0) {
    MergeContacts(vcontact,kContactPosMergeTolerance,kContactOriMergeTolerance);
  }

  if(vcontact.size() > 0) {
    if(numOk != (int)vcontact.size())
    	//// The int type is not guaranteed to be big enough, use intptr_t
		//cout<<numOk<<" contacts between env "<<(int)dGeomGetData(o2)<<" and body "<<(int)dGeomGetData(o1)<<"  (clustered to "<<vcontact.size()<<")"<<endl;
      cout<<numOk<<" contacts between link "<<GeomDataToRobotLinkIndex(dGeomGetData(o2))<<" and link "<<GeomDataToRobotLinkIndex(dGeomGetData(o1))<<"  (clustered to "<<vcontact.size()<<")"<<endl;
    gContacts.push_back(ODEContactResult());
    gContacts.back().o1 = o1;
    gContacts.back().o2 = o2;
    swap(gContacts.back().contacts,vcontact);
    gContacts.back().meshOverlap = !GetCustomGeometryCollisionReliableFlag();
  }
}

void ProcessContacts(list<ODEContactResult>::iterator start,list<ODEContactResult>::iterator end,const ODESimulatorSettings& settings,bool aggregateCount=true)
{
  if(kMergeContacts) {
    for(list<ODEContactResult>::iterator j=start;j!=end;j++) 
      MergeContacts(j->contacts,kContactPosMergeTolerance,kContactOriMergeTolerance);
  }

  static bool warnedContacts = false;
  if(aggregateCount) {
    int numContacts = 0;
    for(list<ODEContactResult>::iterator j=start;j!=end;j++) 
      numContacts += (int)j->contacts.size();
    if(numContacts > settings.maxContacts) {
      //printf("Warning: %d robot-env contacts > maximum %d, may crash\n",numContacts,settings.maxContacts);
      if(settings.maxContacts > 50) {
	if(!warnedContacts) {
	  printf("Max contacts > 50, may crash.  Press enter to continue...\n");
	  getchar();
	}
	warnedContacts = true;
      }
      Real scale = Real(settings.maxContacts)/numContacts;
      for(list<ODEContactResult>::iterator j=start;j!=end;j++) {
	int n=(int)Ceil(Real(j->contacts.size())*scale);
	//printf("Clustering %d->%d\n",j->contacts.size(),n);
	ClusterContacts(j->contacts,n,settings.clusterNormalScale);
      }
    }
  }
  else {
    for(list<ODEContactResult>::iterator j=start;j!=end;j++) {
      if(settings.maxContacts > 50) {
	if(!warnedContacts) {
	  printf("Max contacts > 50, may crash.  Press enter to continue...\n");
	  getchar();
	}
	warnedContacts = true;
      }
      for(list<ODEContactResult>::iterator j=start;j!=end;j++) {
	ClusterContacts(j->contacts,settings.maxContacts,settings.clusterNormalScale);
      }
    }
  }
}

void ODESimulator::ClearCollisions()
{
  dJointGroupEmpty(contactGroupID);
  ClearContactFeedback();
}

void ODESimulator::GetSurfaceParameters(const ODEObjectID& a,const ODEObjectID& b,dSurfaceParameters& surface) const
{
  Assert(timestep > 0);
  //TODO: base the friction on the properties of the contact point
  //completely rigid contact
  surface.mode = dContactApprox1;
  //surface.mode = 0;
  surface.bounce = 0;
  surface.bounce_vel = 0;
  
  //printf("GetSurfaceParameters a = %d,%d, b = %d,%d\n",a.type,a.index,b.type,b.index);
  ODEGeometry *ma,*mb;
  if(a.type == 0) {
    Assert(a.index < (int)terrains.size());
    ma = terrainGeoms[a.index];
  }
  else if(a.type == 1) 
    ma = robots[a.index]->triMesh(a.bodyIndex);
  else if(a.type == 2)
    ma = objects[a.index]->triMesh();
  else Abort();
  if(b.type == 0) {
    Assert(b.index < (int)terrains.size());
    mb=terrainGeoms[b.index];
  }
  else if(b.type == 1) 
    mb=robots[b.index]->triMesh(b.bodyIndex);
  else if(b.type == 2) 
    mb=objects[b.index]->triMesh();
  else Abort();

  const ODESurfaceProperties &propa=ma->surf(),&propb=mb->surf();
  if(!IsInf(propa.kStiffness) || !IsInf(propb.kStiffness)) {
    surface.mode |= (dContactSoftERP | dContactSoftCFM);
    Real kStiffness = 1.0/(1.0/propa.kStiffness+1.0/propb.kStiffness);
    Real kDamping = 1.0/(1.0/propa.kDamping+1.0/propb.kDamping);
    surface.soft_erp = ERPFromSpring(timestep,kStiffness,kDamping);
    surface.soft_cfm = CFMFromSpring(timestep,kStiffness,kDamping);
    //printf("Joint stiffness %g, damping %g, time step %g\n",kStiffness,kDamping,timestep);
    //printf("ERP = %g, CFM = %g\n",surface.soft_erp,surface.soft_cfm);
  }
  surface.mu = 2.0/(1.0/propa.kFriction+1.0/propb.kFriction);
  //correction to account for pyramid shaped friction cone
  surface.mu *= 0.707;
  surface.bounce = 0.5*(propa.kRestitution+propb.kRestitution);
  surface.bounce_vel = 1e-2;
  if(surface.bounce != 0) {
    surface.mode |= dContactBounce;
  }
}

void ODESimulator::SetupContactResponse()
{
  //clear feedback structure
  ClearContactFeedback();
  //clear global ODE collider feedback stuff
  dJointGroupEmpty(contactGroupID);

  int index=0;
  gContactsVector.resize(gContacts.size());
  for(list<ODEContactResult>::iterator i=gContacts.begin();i!=gContacts.end();i++) {
    gContactsVector[index] = &(*i);
    SetupContactResponse(GeomDataToObjectID(dGeomGetData(i->o1)),GeomDataToObjectID(dGeomGetData(i->o2)),index,*i);
    index++;
  }
}

void ODESimulator::SetupContactResponse(const ODEObjectID& a,const ODEObjectID& b,int feedbackIndex,ODEContactResult& c)
{
  dContact contact;
  GetSurfaceParameters(a,b,contact.surface);
  dBodyID b1 = dGeomGetBody(c.o1);
  dBodyID b2 = dGeomGetBody(c.o2);
  c.feedback.resize(c.contacts.size());
  for(size_t k=0;k<c.contacts.size();k++) {
    //add contact joint to joint group
    contact.geom = c.contacts[k];
    Assert(contact.geom.g1 == c.o1 || contact.geom.g1 == c.o2);
    Assert(contact.geom.g2 == c.o1 || contact.geom.g2 == c.o2);
    Assert(contact.geom.depth >= 0);
    
    Assert(contact.geom.g1 == c.o1);
    dJointID joint = dJointCreateContact(worldID,contactGroupID,&contact);
    dJointSetFeedback(joint,&c.feedback[k]);
    //if(b2==0)
    //dJointAttach(joint,b2,b1);
      //else
      dJointAttach(joint,b1,b2);
  }
  //if contact feedback is enabled, do it!
  CollisionPair cindex;
  bool reverse = false;
  if(b < a) {
    cindex.first = b;
    cindex.second = a;
    reverse = true;
  }
  else {
    cindex.first = a;
    cindex.second = b;
  }
  ODEContactList* cl=NULL;
  if(contactList.count(cindex) != 0) {
    cl=&contactList[cindex];
  }
  else {
    //check if there's a generic robot feedback list
    bool checkRobot=false;
    if(cindex.first.type == 1 && cindex.first.bodyIndex != -1) {
      cindex.first.bodyIndex=-1;
      checkRobot=true;
    }
    if(cindex.second.type == 1 && cindex.second.bodyIndex != -1) {
      cindex.second.bodyIndex=-1;
      checkRobot=true;
    }
    if(checkRobot) {
      if(contactList.count(cindex) != 0) 
	cl=&contactList[cindex];
    }
  }
  if(cl) {
    //user requested contact feedback, now copy it out
    size_t start=cl->points.size();
    cl->points.resize(start+c.contacts.size());
    for(size_t k=0;k<c.contacts.size();k++) {
      Assert(k+start < cl->points.size());
      CopyVector(cl->points[k+start].x,c.contacts[k].pos);
      CopyVector(cl->points[k+start].n,c.contacts[k].normal);
      cl->points[k+start].kFriction = contact.surface.mu;
      if(reverse)
	cl->points[k+start].n.inplaceNegative();
    }
    Assert(feedbackIndex >= 0 && feedbackIndex < (int)gContacts.size());
    cl->feedbackIndices.push_back(feedbackIndex);
  }
}

void ODESimulator::DetectCollisions()
{
#if DO_TIMING
  Timer timer;
#endif //DO_TIMING

  gContacts.clear();
  gContactsVector.resize(0);

  CollisionPair cindex;
  int jcount=0;
  if(settings.rigidObjectCollisions) {
    //call the collision routine between objects and the world
    dSpaceCollide(envSpaceID,(void*)this,collisionCallback);

#if DO_TIMING
    gContactDetectTime += timer.ElapsedTime();
    timer.Reset();
#endif //DO_TIMING
    
    ProcessContacts(gContacts.begin(),gContacts.end(),settings,false);

#if DO_TIMING
    gClusterTime += timer.ElapsedTime();
    timer.Reset();
#endif //DO_TIMING
  }

  //do robot-environment collisions
  for(size_t i=0;i<robots.size();i++) {
#if DO_TIMING
    timer.Reset();
#endif //DO_TIMING

    //call the collision routine between the robot and the world
	bool gContactsEmpty = gContacts.empty();
	list<ODEContactResult>::iterator gContactStart;
	if(!gContactsEmpty) gContactStart = --gContacts.end();
	dSpaceCollide2((dxGeom *)robots[i]->space(),(dxGeom *)envSpaceID,(void*)this,collisionCallback);
	if(!gContactsEmpty) ++gContactStart;
	else gContactStart = gContacts.begin();

#if DO_TIMING
    gContactDetectTime += timer.ElapsedTime();
    timer.Reset();
#endif //DO_TIMING

    ProcessContacts(gContactStart,gContacts.end(),settings);

#if DO_TIMING
    gClusterTime += timer.ElapsedTime();
    timer.Reset();
#endif //DO_TIMING

    if(settings.robotSelfCollisions) {
      robots[i]->EnableSelfCollisions(true);

#if DO_TIMING
      timer.Reset();
#endif

      gContactsEmpty = gContacts.empty();
	  if(!gContactsEmpty) gContactStart = --gContacts.end();
      //call the self collision routine for the robot
      dSpaceCollide(robots[i]->space(),(void*)robots[i],selfCollisionCallback);
      if(!gContactsEmpty) ++gContactStart;
	  else gContactStart = gContacts.begin();

#if DO_TIMING
    gContactDetectTime += timer.ElapsedTime();
    timer.Reset();
#endif //DO_TIMING
      
      ProcessContacts(gContactStart,gContacts.end(),settings);

#if DO_TIMING
    gClusterTime += timer.ElapsedTime();
    timer.Reset();
#endif //DO_TIMING
    }

    if(settings.robotRobotCollisions) {    
      for(size_t k=i+1;k<robots.size();k++) {
	cindex.second = ODEObjectID(1,k);

#if DO_TIMING
    timer.Reset();
#endif //DO_TIMING

	gContactsEmpty = gContacts.empty();
	if(!gContactsEmpty) gContactStart = --gContacts.end();
	dSpaceCollide2((dxGeom *)robots[i]->space(),(dxGeom *)robots[k]->space(),(void*)this,collisionCallback);
	if(!gContactsEmpty) ++gContactStart;
	else gContactStart = gContacts.begin();

#if DO_TIMING
    gContactDetectTime += timer.ElapsedTime();
    timer.Reset();
#endif //DO_TIMING

	ProcessContacts(gContactStart,gContacts.end(),settings);

#if DO_TIMING
    gClusterTime += timer.ElapsedTime();
    timer.Reset();
#endif //DO_TIMING
      }
    }
  }
}

void ODESimulator::EnableContactFeedback(const ODEObjectID& a,const ODEObjectID& b)
{
  CollisionPair index;
  if(a < b) {
    index.first=a;
    index.second=b;
  }
  else {
    index.first=b;
    index.second=a;
  }
  contactList[index] = ODEContactList();
}

ODEContactList* ODESimulator::GetContactFeedback(const ODEObjectID& a,const ODEObjectID& b)
{
  CollisionPair index;
  if(a < b) {
    index.first=a;
    index.second=b;
  }
  else {
    index.first=b;
    index.second=a;
  }
  if(contactList.count(index) != 0) {
    return &contactList[index];
  }
  return NULL;
}

bool HasContact(dBodyID a)
{
  if(a == 0) return false;
  int n = dBodyGetNumJoints (a);
  for(int i=0;i<n;i++) {
    dJointID j=dBodyGetJoint (a,i);
    if(dJointGetType(j)==dJointTypeContact) return true;
  }
  return false;
}

///Will produce bogus o1 and o2 vectors
void GetContacts(dBodyID a,vector<ODEContactList>& contacts)
{
  if(a == 0) return;

  contacts.resize(0);
  for(list<ODEContactResult>::iterator i=gContacts.begin();i!=gContacts.end();i++) {
    if(a == dGeomGetBody(i->o1) || a == dGeomGetBody(i->o2)) {
      dBodyID b = dGeomGetBody(i->o2);
      bool reverse = false;
      if(b == a) { b = dGeomGetBody(i->o1); reverse = true; }
      contacts.resize(contacts.size()+1);
      contacts.back().penetrating = i->meshOverlap;
      contacts.back().points.resize(i->contacts.size());
      contacts.back().forces.resize(i->feedback.size());
      for(size_t j=0;j<i->feedback.size();j++) {
	CopyVector(contacts.back().forces[j],i->feedback[j].f1);
	CopyVector(contacts.back().points[j].x,i->contacts[j].pos);
	CopyVector(contacts.back().points[j].n,i->contacts[j].normal);
	//contacts.back().points[j].kFriction = i->contacts[j].surface.mu;
	contacts.back().points[j].kFriction = 0;
	if(reverse) {
	  contacts.back().forces[j].inplaceNegative();
	  contacts.back().points[j].n.inplaceNegative();
	}
      }
    }
  }
}


bool HasContact(dBodyID a,dBodyID b)
{
  if(a == 0 && b == 0) return false; //two terrains
  if(a == 0) Swap(a,b);
  int n = dBodyGetNumJoints (a);
  for(int i=0;i<n;i++) {
    dJointID j=dBodyGetJoint (a,i);
    if(dJointGetType(j)==dJointTypeContact) {
      dBodyID j0=dJointGetBody(j,0);
      dBodyID j1=dJointGetBody(j,1);
      if(j0 == b || j1 == b) return true;
    }
  }
  return false;
}

bool ODESimulator::InContact(const ODEObjectID& a) const
{
  if(a.type == 0) { //terrain
    //must loop through all objects to see what is in contact with the
    //environment
    for(size_t i=0;i<objects.size();i++) {
      if(HasContact(objects[i]->body(),0)) return true;
    }
    for(size_t i=0;i<robots.size();i++) {
      for(size_t j=0;j<robots[i]->robot.links.size();j++) {
	if(HasContact(robots[i]->body(j),0)) return true;
      }
    }    
    return false;
  }
  else if(a.type == 2) {//object
    return HasContact(objects[a.index]->body());
  }
  else {
    if(a.bodyIndex >= 0)
      return HasContact(robots[a.index]->body(a.bodyIndex));
    else { //any robot link
      for(size_t i=0;i<robots[a.index]->robot.links.size();i++)
	if(HasContact(robots[a.index]->body(i))) return true;
      return false;
    }
  }
  return false;
}

bool ODESimulator::InContact(const ODEObjectID& a,const ODEObjectID& b) const
{
  vector<dBodyID> bodya, bodyb;
  if(a.type == 0) {
    bodya.push_back(0);
  }
  else if(a.type == 2) {
    bodya.push_back(objects[a.index]->body());
  }
  else {
    if(a.bodyIndex >= 0)
      bodya.push_back(robots[a.index]->body(a.bodyIndex));
    else {
      for(size_t i=0;i<robots[a.index]->robot.links.size();i++)
	if(robots[a.index]->body(i))
	  bodya.push_back(robots[a.index]->body(i));
    }
  }
  if(b.type == 0) {
    bodyb.push_back(0);
  }
  else if(b.type == 2) {
    bodyb.push_back(objects[b.index]->body());
  }
  else {
    if(b.bodyIndex >= 0)
      bodyb.push_back(robots[b.index]->body(b.bodyIndex));
    else {
      for(size_t i=0;i<robots[b.index]->robot.links.size();i++)
	if(robots[b.index]->body(i))
	  bodyb.push_back(robots[b.index]->body(i));
    }
  }
  for(size_t i=0;i<bodya.size();i++)
    for(size_t j=0;j<bodyb.size();j++)
      if(HasContact(bodya[i],bodyb[j])) return true;
  return false;
}

void ODESimulator::StepDynamics(Real dt)
{
  dWorldStep(worldID,dt);
  //dWorldQuickStep(worldID,dt);
}


bool ODESimulator::ReadState(File& f)
{
#if TEST_READ_WRITE_STATE
  for(size_t i=0;i<robots.size();i++) 
    TestReadWriteState(*robots[i],"ODERobot");
  for(size_t i=0;i<objects.size();i++) 
    TestReadWriteState(*objects[i],"ODEObject");
#endif

  for(size_t i=0;i<robots.size();i++) {
    if(!robots[i]->ReadState(f)) {
      fprintf(stderr,"ODESimulator::ReadState(): failed to read robot %d\n",i);
      return false;
    }
  }
  for(size_t i=0;i<objects.size();i++) {
    if(!objects[i]->ReadState(f)) {
      fprintf(stderr,"ODESimulator::ReadState(): failed to read object %d\n",i);
      return false;
    }
  }
  ClearContactFeedback();
  return true;
}

void ODESimulator::ClearContactFeedback()
{
  for(map<CollisionPair,ODEContactList>::iterator i=contactList.begin();i!=contactList.end();i++) {
    i->second.points.clear();
    i->second.forces.clear();
    i->second.feedbackIndices.clear();
  }
}

bool ODESimulator::WriteState(File& f) const
{
  for(size_t i=0;i<robots.size();i++) 
    if(!robots[i]->WriteState(f)) return false;
  for(size_t i=0;i<objects.size();i++) 
    if(!objects[i]->WriteState(f)) return false;
  return true;
}
