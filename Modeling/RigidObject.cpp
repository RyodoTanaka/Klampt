#include "RigidObject.h"
#include <KrisLibrary/Timer.h>
#include "Mass.h"
#include "IO/ROS.h"
#include <KrisLibrary/robotics/Inertia.h>
#include <KrisLibrary/utils/SimpleFile.h>
#include <KrisLibrary/utils/stringutils.h>
#include <KrisLibrary/GLdraw/GL.h>
#include <KrisLibrary/GLdraw/drawextra.h>
#include <KrisLibrary/meshing/IO.h>
#include <string.h>
#include <fstream>
using namespace Math3D;
using namespace std;

RigidObject::RigidObject()
{
  T.setIdentity();
  mass = 1;
  com.setZero();
  inertia.setIdentity();
  kFriction = 0.5;
  kRestitution = 0.0;
  kStiffness = kDamping = Inf;
}


bool RigidObject::Load(const char* fn)
{
  const char* ext=FileExtension(fn);
  if(ext && strcmp(ext,"obj")==0) {
    SimpleFile f;
    f.AllowItem("mesh");
    f.AllowItem("geomscale");
    f.AllowItem("geomtranslate");
    f.AllowItem("T");
    f.AllowItem("mass");
    f.AllowItem("inertia");
    f.AllowItem("com");
    f.AllowItem("kFriction");
    f.AllowItem("kRestitution");
    f.AllowItem("kStiffness");
    f.AllowItem("kDamping");
    f.AllowItem("autoMass");
    if(!f.Load(fn)) return false;

    if(!f.CheckSize("mesh",1,fn)) return false;
    if(!f.CheckType("mesh",PrimitiveValue::String,fn)) return false;

    string fnPath = GetFilePath(fn);
    geomFile = f["mesh"][0].AsString();
    string geomfn = fnPath + geomFile;
    if(!LoadGeometry(geomfn.c_str()))
      return false;
    f.erase("mesh");

    Matrix4 ident; ident.setIdentity();
    Matrix4 geomT=ident; 
    if(f.count("geomscale") != 0) {
      if(!f.CheckType("geomscale",PrimitiveValue::Double,fn)) return false;
      vector<double> scale = f.AsDouble("geomscale");
      if(scale.size()==1) { geomT(0,0)=geomT(1,1)=geomT(2,2)=scale[0]; }
      else if(scale.size()==3) {
	geomT(0,0)=scale[0];
	geomT(1,1)=scale[1];
	geomT(2,2)=scale[2];
      }
      else {
	fprintf(stderr,"Invalid number of geomscale components in %s\n",fn);
	return false;
      }
      f.erase("geomscale");
    }
    if(f.count("geomtranslate") != 0) {
      if(!f.CheckType("geomtranslate",PrimitiveValue::Double,fn)) return false;
      if(!f.CheckSize("geomtranslate",3,fn)) return false;
      vector<double> trans = f.AsDouble("geomtranslate");
      geomT(0,3)=trans[0];
      geomT(1,3)=trans[1];
      geomT(2,3)=trans[2];
      f.erase("geomtranslate");
    }
    if(!(ident == geomT)) {
      geometry.TransformGeometry(geomT);  
    }
    if(f.count("T")==0) { T.setIdentity(); }
    else {
      if(!f.CheckType("T",PrimitiveValue::Double,fn)) return false;
      vector<double> items = f.AsDouble("T");
      if(items.size()==12) { //read 4 columns of 3
	Vector3 x(items[0],items[1],items[2]);
	Vector3 y(items[3],items[4],items[5]);
	Vector3 z(items[6],items[7],items[8]);
	Vector3 t(items[9],items[10],items[11]);
	T.R.set(x,y,z); T.t=t;
      }
      else if(items.size()==16) { //read 4 columns of 4
	Vector3 x(items[0],items[1],items[2]);
	Vector3 y(items[4],items[5],items[6]);
	Vector3 z(items[8],items[9],items[10]);
	Vector3 t(items[12],items[13],items[14]);
	T.R.set(x,y,z); T.t=t;
      }
      else {
	fprintf(stderr,"Invalid number of transformation components in %s\n",fn);
	return false;
      }
      f.erase("T");
    }
    if(f.count("mass")==0) { mass=1.0;  }
    else {
      if(!f.CheckSize("mass",1)) return false;
      if(!f.CheckType("mass",PrimitiveValue::Double)) return false;
      mass = f["mass"][0].AsDouble();
      f.erase("mass");
    }
    bool hasCOM = false;
    if(f.count("com")==0) { com.setZero();  }
    else {
      if(!f.CheckSize("com",3)) return false;
      if(!f.CheckType("com",PrimitiveValue::Double)) return false;
      hasCOM = true;
      com.set(f["com"][0].AsDouble(),f["com"][1].AsDouble(),f["com"][2].AsDouble());
      f.erase("com");
    }
    if(f.count("inertia")==0) inertia.setIdentity();
    else {
      if(!f.CheckType("inertia",PrimitiveValue::Double,fn)) return false;
      vector<double> items = f.AsDouble("inertia");
      if(items.size() == 3) inertia.setDiagonal(Vector3(items[0],items[1],items[2]));
      else if(items.size() == 9) {
	inertia(0,0)=items[0]; 	inertia(0,1)=items[1]; 	inertia(0,2)=items[2];
	inertia(1,0)=items[3]; 	inertia(1,1)=items[4]; 	inertia(1,2)=items[5];
	inertia(2,0)=items[6]; 	inertia(2,1)=items[7]; 	inertia(2,2)=items[8];
      }
      else {
	fprintf(stderr,"Invalid number of inertia matrix components in %s\n",fn);
	return false;
      }
      f.erase("inertia");
    }
    if(f.count("kFriction")==0) kFriction = 0.5;
    else {
      if(!f.CheckSize("kFriction",1,fn)) return false;
      if(!f.CheckType("kFriction",PrimitiveValue::Double,fn)) return false;
      kFriction = f.AsDouble("kFriction")[0];
      f.erase("kFriction");
    }
    if(f.count("kRestitution")==0) kRestitution = 0.5;
    else {
      if(!f.CheckSize("kRestitution",1,fn)) return false;
      if(!f.CheckType("kRestitution",PrimitiveValue::Double,fn)) return false;
      kRestitution = f.AsDouble("kRestitution")[0];
      f.erase("kRestitution");
    }
    if(f.count("kStiffness")==0) kStiffness=Inf;
    else {
      if(!f.CheckSize("kStiffness",1,fn)) return false;
      if(!f.CheckType("kStiffness",PrimitiveValue::Double,fn)) return false;
      kStiffness = f.AsDouble("kStiffness")[0];
      f.erase("kStiffness");
    }
    if(f.count("kDamping")==0) kDamping=Inf;
    else {
      if(!f.CheckSize("kDamping",1,fn)) return false;
      if(!f.CheckType("kDamping",PrimitiveValue::Double,fn)) return false;
      kDamping = f.AsDouble("kDamping")[0];
      f.erase("kDamping");
    }
    if(f.count("autoMass")!=0) {
      if(hasCOM) //com specified, compute inertia about given com
	inertia = Inertia(*geometry,com,mass);
      else
	SetMassFromGeometry(mass);
      f.erase("autoMass");
    }
    if(!f.empty()) {
      for(map<string,vector<PrimitiveValue> >::const_iterator i=f.entries.begin();i!=f.entries.end();i++)
	fprintf(stderr,"Unknown entry %s in object file %s\n",i->first.c_str(),fn);
    }
    return true;
  }
  else {
    if(!LoadGeometry(fn)) {
      //printf("LoadGeometry %s failed\n",fn);
      return false;
    }
    T.setIdentity();
    mass=1.0;
    com.setZero();
    inertia.setZero();
    kFriction = 0.5;
    kRestitution = 0.5;
    kStiffness=Inf;
    kDamping=Inf;
    //if(ext)
    //  fprintf(stderr,"Warning, loading object from .%s file %s.  Setting COM and inertia matrix from geometry.\n",ext,fn);
    //else
    //  fprintf(stderr,"Warning, loading object from file %s.  Setting COM and inertia matrix from geometry.\n",fn);
    SetMassFromGeometry(1.0);
    return true;
  }
}

bool RigidObject::LoadGeometry(const char* fn)
{
  geomFile = fn;
  //default appearance options
  geometry.Appearance()->faceColor.set(0.8,0.8,0.8);
  if(geometry.Load(geomFile)) {
    return true;
  }
  return false;
}

bool RigidObject::Save(const char* fn)
{
  ofstream out(fn);
  if(!out) return false;
  fprintf(stderr,"RigidObject::Save: not done yet\n");
  return false;
  return true;
}

void RigidObject::SetMassFromGeometry(Real totalMass)
{
  mass = totalMass;
  com = CenterOfMass(*geometry);
  inertia = Inertia(*geometry,com,mass);
}

void RigidObject::SetMassFromBB(Real totalMass)
{
  AABB3D bb=geometry->GetAABB();
  mass = totalMass;
  com = 0.5*(bb.bmin+bb.bmax);
  BoxInertiaMatrix(bb.bmax.x-bb.bmin.x,bb.bmax.y-bb.bmin.y,bb.bmax.z-bb.bmin.z,mass,inertia);
}

void RigidObject::InitCollisions()
{
  Timer timer;
  geometry->InitCollisionData();
  double t = timer.ElapsedTime();
  if(t > 0.2) 
    printf("Initialized rigid object %s collision data structures in time %gs\n",geomFile.c_str(),t);
}

void RigidObject::UpdateGeometry()
{
  geometry->SetTransform(T);
}

void RigidObject::DrawGL()
{
  if(!geometry) return;

  glDisable(GL_CULL_FACE);
  glPushMatrix();
  GLDraw::glMultMatrix(Matrix4(T));

  geometry.DrawGL();

  glPopMatrix();
  glEnable(GL_CULL_FACE);
}
