#include "faunus/moves/translational.h"
#include "faunus/io.h"
#include "faunus/energy/base.h"
#include "faunus/ensemble.h"

namespace Faunus {

  zmove::zmove( ensemble &e, container &c, energybase &i) : markovmove(e,c,i) {
    runfraction=1;
    dp=8;
    deltadp=1;
    name.assign("MACROMOLECULE Z-DISPLACEMENTS");
  }     

  double zmove::move(macromolecule &g) {
    rc=OK;
    du=0;
    if (slp.runtest(runfraction)==false)
      return du;
    cnt++;
    z=2*dp*slp.random_half();
    g.zmove(*con, z);
    for (int i=g.beg; i<(g.size()+g.beg); i++) { 
      if (con->collision( con->trial[i] )==true || ( g.cm_trial.z>zmax && z>0 ) )
        rc=HC;
    }
    if (rc==HC) {
      g.undo(*con);
      return 0; }
    else {
      //#pragma omp parallel
      {
        //#pragma omp sections
        {
          //#pragma omp section
          { uold = pot->energy(con->p, g);   }
          //#pragma omp section
          { unew = pot->energy(con->trial,g);   }
        }
      }
      du += unew - uold; }
      if (ens->metropolis(du)==true) {
        rc=OK;
        utot+=du;
        naccept++;
        dpsqr+=z*z; 
        g.accept(*con);
        return du;
      } else rc=ENERGY;

      du=0;
      g.undo(*con);
      return du;
  }

  /*!
   * \todo "gofr" constructor should be called with dynamically loaded values, not fixed ones
   */
  dualmove::dualmove( ensemble &e,
      container &c, energybase &i ) : markovmove(e,c,i), gofr(0.1,0.,1500.)
  {
    name.assign("SYMMETRIC 1D GROUP TRANSLATION");
    cite.assign("Biophys J. 2003, 85, 2940");
    runfraction=1.0;
    deltadp=1.;
    dp=3.;
    direction(0,0,1); // move only in z direction
    rmin=0;
    rmax=pow(double(c.getvolume()), 1./3)/2.; // rough estimate from volume
  }

  /*! Load the following parameters from an input
   * object: displacement (dm_dp), minimum separation
   * (dm_minsep), maximum separation (dm_maxsep)
   */
  void dualmove::setup( inputfile &in ) {
    prefix="dm_";
    markovmove::getInput(in);
    rmin = in.getflt(prefix+"minsep",rmin);
    rmax = in.getflt(prefix+"maxsep",rmax);
  }

  /*! Specify unit vector that determines which coordinates
   *  that will be moved. The default is (0,0,1) meaning that
   *  the macromolecules will be moved in the z direction only.
   */
  void dualmove::direction(double x, double y, double z) {
    v.x=x;
    v.y=y;
    v.z=z;
  }

  /*!
   * Loads two macromolecules from disk and place them symmetrically
   * around the cell origin, separated by a specified distance.
   *
   * \param in Inputfile object that contains the protein filenames
   * \param g Macromolecular vector (will be erased!)
   * \param dist Initial distance between the protein mass-centers
   * \note The macromolecule vector is erased before any proteins are loaded!
   */
  void dualmove::load(inputfile &in, vector<macromolecule> &g, float dist) {
    if (dist==0)
      dist=rmax;
    g.clear();
    point a = v*(dist/2.);
    ioaam aam;
    aam.load(*con, in, g); 
    g[0].move(*con, -( g[0].cm + a ));    // ...around the cell origo
    g[1].move(*con, -( g[1].cm - a ));    // ...along the z-axis
    g[0].accept(*con);
    g[1].accept(*con);
  }

  string dualmove::info() {
    std::ostringstream o;
    o <<  markovmove::info()
      << "#   Min/max separation        = " << rmin << " " << rmax << endl;
    return o.str();
  }

  double dualmove::move(macromolecule &g1, macromolecule &g2) {
    if (dp==0)
      return 0;
    markovmove::move();
    r=con->dist(g1.cm, g2.cm);
    point p;
    group g12=g1+g2;
    p.x=v.x*dp*slp.random_half();
    p.y=v.y*dp*slp.random_half();
    p.z=v.z*dp*slp.random_half();
    g1.move(*con, p);
    g2.move(*con,-p);
    double rtrial = con->dist(g1.cm_trial,g2.cm_trial);
    if (con->collision(g1.cm_trial)==true ||
        con->collision(g2.cm_trial)==true || 
        rtrial > rmax || rtrial < rmin) {
      rc=HC;
      dpsqr+=0;
      g1.undo(*con);
      g2.undo(*con);
      gofr.add(r);
      return du;
    }
    //#pragma omp parallel
    {
      //#pragma omp sections
      {
        //#pragma omp section
        { uold = pot->energy(con->p,g12) + pot->energy(con->p,g1,g2);   }
        //#pragma omp section
        { unew = pot->energy(con->trial,g12) + pot->energy(con->trial,g1,g2);   }
      }
    }
    du = unew-uold;
    if (ens->metropolis(du)==true) {
      rc=OK;
      utot+=du;
      naccept++;
      g1.accept(*con);
      g2.accept(*con);
      r=con->dist(g1.cm, g2.cm);
      dpsqr+=r*r;
      gofr.add(r);
      return du;
    } else
      rc=ENERGY;
    du=0;
    dpsqr+=0;
    g1.undo(*con);
    g2.undo(*con);
    gofr.add(r);
    return du;
  }

  //---------- TRANSLATE MOLECULE ----------------
  translate::translate( ensemble &e,
      container &c, energybase &i, inputfile &in) : markovmove(e,c,i) {
    name.assign("MOLECULAR TRANSLATION");
    prefix.assign("moltrans_");
    runfraction=1.0;
    deltadp=1.;
    dp=10.;
    dpv.x=dpv.y=dpv.z=1.;
    markovmove::getInput(in);
  }

  string translate::info() {
    ostringstream o;
    o << markovmove::info()
      << "#   Displacement directions   = " << dpv.x << " " << dpv.y << " " << dpv.z << endl;
    return o.str();
  }

  double translate::move(group &g) {
    if (slp.runtest(runfraction)==false)
      return 0;
    markovmove::move();
    point p;
    p.x=dpv.x*dp*slp.random_half();
    p.y=dpv.y*dp*slp.random_half();
    p.z=dpv.z*dp*slp.random_half();
    g.move(*con, p); 
    
    bool hc=false;
    g.cm_trial = g.masscenter(*con, con->trial);
    if (con->slicecollision(g.cm_trial)==true) 
      hc=true;
    for (int i=g.beg; i<=g.end; i++) {
      if (con->collision(con->trial[i])==true) {
        hc=true;
        break;
      }
    }
    if (hc==true) {
      rc=HC;
      dpsqr+=0;
      g.undo(*con);
      return du;
    }
    //#pragma omp parallel
    {
      //#pragma omp sections
      {
        //#pragma omp section
        { uold = pot->energy(con->p, g);   }
        //#pragma omp section
        { unew = pot->energy(con->trial, g);   }
      }
    }
    du = unew-uold;
    if (ens->metropolis(du)==true) {
      rc=OK;
      utot+=du;
      dpsqr+=con->sqdist( g.cm, g.cm_trial );
      naccept++;
      g.accept(*con);
      return du;
    } else rc=ENERGY;
    du=0;
    dpsqr+=0;
    g.undo(*con);
    return du;
  }

  //-----------MOVE----------------------------------------
  /*  \brief Class to prefom a random walk of a macromolecule in space
   *  \note Replaced by translate(?)
   *
  move::move(
      ensemble &e, container &c, energybase &i, box &BOX,float rf) : markovmove(e,c,i) {
    runfraction=rf;
    dp=100;
    deltadp=1;
    name.assign("MACROMOLECULE MOVE");
    b=&BOX;
  }

  bool move::mOve(macromolecule &g) {
    du=0;
    if (slp.runtest(runfraction)==false)
      return false;
    cnt++;
    point rand;
    rand.x = dp*slp.random_half();
    rand.y = dp*slp.random_half();
    rand.z = dp*slp.random_half();
    g.move(*con, rand);
    for (int i=g.beg; i<(g.size()+g.beg); i++) { 
      b->boundary(con->trial[i]);
      if (con->collision( con->trial[i] )==true) 
        rc=HC;
    }
    if (rc==HC) {
      for (int i=g.beg; i<(g.size()+g.beg); i++) 
        con->trial[i] = con->p[i];
      std::cout << "rejected"<<endl;
      return false; }
    else {
      //#pragma omp parallel
      {
        //#pragma omp sections
        {
          //#pragma omp section
          { uold = pot->energy(con->p, g);   }
          //#pragma omp section
          { unew = pot->energy(con->trial,g);   }
        }
      }
      du += unew - uold; }
      if (ens->metropolis(du)==true) {
        rc=OK;
        utot+=du;
        naccept++;
        for (int i=g.beg; i<(g.beg+g.size()); i++)
          con->p[i] = con->trial[i];
        return true;
      } else rc=ENERGY;

      du=0;
      for (int i=g.beg; i<(g.size()+g.beg); i++) 
        con->trial[i] = con->p[i];
      return false;
  }
*/

  //-------------- SALT MOVE ---------------------------------
  saltmove::saltmove(ensemble &e, container &c, energybase &i) : markovmove(e,c,i) {
    init();
  }

  /*!
   * \param e         Ensemble class
   * \param c         Container class
   * \param i         Interaction energy class
   * \param in        Inputfile object
   * \param p         Prefix for keywords in inputfile
   */
  saltmove::saltmove(ensemble &e, container &c, energybase &i, inputfile &in, string p) : markovmove(e,c,i) {
    prefix=p;
    init();
    markovmove::getInput(in);
    if (dp<1e-5)
      dp=in.getflt("dp_salt", 40.);  // Backwards compatibility...
  }
  
  void saltmove::init() {
    name.assign("SALT DISPLACEMENTS");
    if (prefix.empty()==true)
      prefix="saltmove_";
    deltadp=2;
    runfraction=1.0;
    rsqr=0;
    dpv.x=dpv.y=dpv.z=1;
  }

  /*! \param g Group containing mobile ions
  */
  double saltmove::move(group &g) {
    du=0;
    if (slp.runtest( runfraction )==false)
      return du;
    double sum=0;
    for (int i=0; i<g.size(); i++) {
      move(g, 1);
      sum+=du;
    }
    du=sum;
    return du;
  }

  double saltmove::move(group &g, int n) {
    if (g.size()==0 || dp<1e-5)
      return 0;
    markovmove::move();
    n=g.displace(*con, dpv*dp); 
    //std::swap(con->p[n], con->p[0]);
    //std::swap(con->trial[n], con->trial[0]);
    //n=0;
    if (con->collision( con->trial[n] )==true)
      rc=HC;
    else {
      //#pragma omp parallel
      {
        //#pragma omp sections
        {
          //#pragma omp section
          { uold = pot->energy(con->p, n);   }
          //#pragma omp section
          { unew = pot->energy(con->trial,n);   }
        }
      }
      du = unew - uold;
      if (ens->metropolis(du)==true) {
        rc=OK;
        utot+=du;                               // track energy changes
        double d2=con->sqdist(con->p[n],con->trial[n]); // track avg. displacement
        dpsqr+=d2;
        rsqr+=d2/(g.size()*g.size());           // Track mean square displacement per particle
        naccept++;                              // accept counter
        con->p[n] = con->trial[n];              // Accept move
        //std::swap(con->p[n], con->p[0]);
        //std::swap(con->trial[n], con->trial[0]);
        return du;
      } else rc=ENERGY;
    }
    du=0;
    dpsqr+=0;
    //std::swap(con->p[n], con->p[0]);
    //std::swap(con->trial[n], con->trial[0]);
    con->trial[n] = con->p[n]; // Revert move
    return du;
  }

  string saltmove::info() {
    std::ostringstream o;
    o << markovmove::info()
      << "#   Total displ./particle (A) = " << sqrt(rsqr) << endl
      << "#   Displacement directions   = " << dpv.x << " " << dpv.y << " " << dpv.z << endl;
    return o.str();
  }

  //------------------------------------------------

  monomermove::monomermove(
      ensemble &e, container &c, energybase &i, inputfile &in, string p) : saltmove(e,c,i) {
    init();
    if (p.empty()==true)
      prefix.assign("monomer_");
    else
      prefix.assign(p);
    name.assign("MONOMER DISPLACEMENTS");
    markovmove::getInput(in);
    if (dp<1e-5)
      dp=in.getflt("dp_monomer", 3.); // backwards compatibility...
  }

  double monomermove::move(polymer &g) {
    if (slp.runtest( runfraction )==false || g.size()==0)
      return 0;
    markovmove::move();
    int n=g.displace(*con, dpv*dp); 

    bool hc=false;
    g.cm_trial = g.masscenter(*con, con->trial);
    if (con->slicecollision(g.cm_trial)==true) 
      hc=true;
    if (con->collision( con->trial[n] )==true)
      hc=true;
    if (hc==true) 
      rc=HC;
    else {
      //uold = pot->energy(con->p, g) + pot->uself_polymer(con->p, g);
      //unew = pot->energy(con->trial, g) + pot->uself_polymer(con->trial, g);
      uold = pot->u_monomer(con->p, g, n);   
      unew = pot->u_monomer(con->trial, g, n);   
      du = unew - uold;
      if (ens->metropolis(du)==true) {
        rc=OK;
        utot+=du;                               // track energy changes
        double d2=con->sqdist(con->p[n],con->trial[n]); // track avg. displacement
        dpsqr+=d2;
        rsqr+=d2/(g.size()*g.size());           // Track mean square displacement per particle
        naccept++;                              // accept counter
        con->p[n] = con->trial[n];              // Accept move
        g.masscenter(*con);                     // Recalc mass center for cm and cm_trial
        return du;
      } else rc=ENERGY;
    }
    du=0;
    dpsqr+=0;
    con->trial[n] = con->p[n];
    g.cm_trial=g.cm;
    return du;
  }
}//namespace

