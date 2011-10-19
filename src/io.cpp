#include <faunus/io.h>
#include <faunus/species.h>
#include <faunus/geometry.h>
#include <faunus/point.h>
#include <faunus/group.h>
#include <faunus/inputfile.h>
#include <faunus/space.h>
#include <faunus/bonded.h>

namespace Faunus {
  /*!
   * The first line in the AAM file format is the number of
   * particles. The following lines defines the particles according
   * to: \n
   *   name num x y z charge weight radius
   */

  bool io::readfile(string file, vector<string> &v) {
    std::ifstream f(file.c_str() );
    if (f) {
      while (getline(f,s))
        v.push_back(s);
      f.close();
      return true;
    }
    std::cout << "# WARNING! FILE " << file << " NOT READ!\n";
    return false;
  }
  
  /*!
   * \param file Filename
   * \param s String to write
   * \param mode std::ios_base::out (new file, default) or std::ios_base::app (append)
   */
  bool io::writefile(string file, string s, std::ios_base::openmode mode) {
    std::ofstream f(file.c_str(), mode);
    if (f) {
      f << s;
      f.close();
      return true;
    }
    return false;
  }
  
  void io::strip(vector<string> &v, string pat) {
    vector<string>::iterator iter=v.begin();
    while (iter!=v.end())
      if ((*iter).find(pat)!=string::npos)
        v.erase(iter);
      else iter++;
  }
  
  void io::splash(string f) {
    vector<string> t;
    t.clear();
    readfile(f,t);
    for (auto t_i : t)
      std::cout << "# " << t_i <<endl;
  }

  //--------------- IOAAM ---------------------

  ioaam::ioaam() {}

  string ioaam::p2s(particle &p, int i) {
    std::ostringstream o;
    o.precision(5);
    o << atom[p.id].name << " " << i+1 << " " << p.x << " " << p.y <<" "<< p.z << " "
      << p.charge << " " << p.mw << " " << p.radius << std::endl;
    return o.str();
  }
  
  particle ioaam::s2p(string &s) {
    particle p;
    std::stringstream o;
    string name, num;
    o << s;
    o >> name >> num >> p.x >> p.y >> p.z >> p.charge >> p.mw >> p.radius;
    p.id = atom[name].id;
    p.hydrophobic = atom[p.id].hydrophobic;
    return p;
  }
  
  bool ioaam::load(string file) {
    vector<string> v;
    p.clear();
    if (fio.readfile(file,v)==true) {
      fio.strip(v,"#");
      unsigned int n=atoi(v[0].c_str());
      for (unsigned int i=1; i<=n; i++)
        p.push_back( s2p(v[i]) );
      return true;
    }
    return false;
  }

  bool ioaam::save(string file, p_vec &p) {
    std::ostringstream o;
    o << p.size() << std::endl;
    for (size_t i=0; i<p.size(); i++)
      o << p2s(p[i], i);
    return fio.writefile(file, o.str());
  }

/*
  //----------------- IOXYZ ----------------------
  ioxyz::ioxyz() {}
  particle ioxyz::s2p(string &s) {
    particle p;
    std::stringstream o;
    string name, num;
    o << s;
    o >> name >> num >> p.x >> p.y >> p.z >> p.charge >> p.mw >> p.radius;
    p.id = atom[name].id;
    p.hydrophobic = atom[p.id].hydrophobic;
    return p;
  }

  bool ioxyz::save(string file, p_vec &p) {
    std::ostringstream o;
    o << p.size() << std::endl << std::endl;
    for (unsigned short i=0; i<p.size(); i++)
      o << atom[p[i].id].name << " "
        << p[i].x << " " << p[i].y << " " << p[i].z << std::endl;
    o << std::endl;
    return writefile(file, o.str());
  }

  p_vec ioxyz::load(string file) {
    v.resize(0);
    particle dummy;

    if (readfile(file,v)==true) {
      strip(v,"#");
      unsigned short n=atoi(v[0].c_str());
      if (n!=sys->p.size()) {
        std::cerr << "!! System vector and .coord vector are out of sync! !!\n"
          << "!! Closing the factory since n=" <<n<< "and p.size() ="<<sys->p.size()<<" !!\n"
          << std::endl;
      }else{ 
        for (unsigned short i=1; i<=n; i++) {
          sys->p[i-1]=s2p(v[i]);
          sys->p[i-1].charge=s2p(v[i]).charge;
          sys->p[i-1].radius=s2p(v[i]).radius;
          sys->p[i-1].mw=s2p(v[i]).mw;
        }
        std::cout << "# Old coordinates loaded\n";
      }
    } else{ std::cout << "# Can't find .coord${jobid} \n";}
    return p;
  }
  */

  /*!
   * Saves particles as a PQR file. This format is very simular
   * to PDB but also contains charges and radii of the proteins.
   */
  iopqr::iopqr() { }

  bool iopqr::save(string file, p_vec &p) {
    string name;
    int nres=1, natom=1;
    char buf[100];
    std::ostringstream o;
    // index, atom->name, atom->resname, atom->resid,x, y, z, atom->charge, atom->radius
    for (auto &p_i : p) {
      name=atom[p_i.id].name;
      sprintf(buf, "ATOM  %5d %-4s %s %5d    %8.3f %8.3f %8.3f %.3f %.3f\n",
          natom++, name.c_str(), name.c_str(), nres,
          p_i.x, p_i.y, p_i.z, p_i.charge, p_i.radius );
      o << buf;
      if ( atom[p_i.id].name=="CTR" ) nres++;
    }
    return fio.writefile(file, o.str());
  }

  /*
  bool iopqr::save(string file, p_vec &p, titrate &tit) {
    p_vec tmp = p;
    for (unsigned short i=0; i<p.size(); i++)
      tmp[i].charge = tit.avgcharge(p, i);
    return save(file, tmp);
  }

  bool iopqr::save(string file, p_vec &p, vector<group> &g) {
    p_vec t;
    for (int i=0; i<g.size(); i++)
      for (int j=g[i].beg; j<=g[i].end; j++)
        t.push_back( p[j] );
    return save(file, t);
  }
  */

  bool iogro::save(string file, p_vec &p) {
    string name;
    int nres=1, natom=1;
    char buf[79];
    double halflen=len/2;
    std::ostringstream o;
    o << "# Generated by Faunus -- http://faunus.sourceforge.net"
      << std::endl << p.size() << std::endl;
    for (auto &pi : p) {
      name=atom[pi.id].name;
      sprintf(buf, "%5d%5s%5s%5d%8.3f%8.3f%8.3f\n",
          nres,name.c_str(),name.c_str(),natom++,
          pi.x/10+halflen, pi.y/10+halflen, pi.z/10+halflen );
      o << buf;
      if ( atom[pi.id].name=="CTR" )
        nres++;
    }
    if (len>0)
      o << len << " " << len << " " << len << std::endl;
    return fio.writefile(file, o.str());
  }

  particle iogro::s2p(string &s) {
    std::stringstream o;
    string name;
    double x,y,z;
    o << s.substr(10,5) << s.substr(20,8) << s.substr(28,8) << s.substr(36,8);
    o >> name >> x >> y >> z;
    particle p;
    p=atom[name]; 
    p.x=x*10; // nm->angstrom
    p.y=y*10;
    p.z=z*10;
    return p;
  }
  
  bool iogro::load(string file) {
    p.clear();
    v.resize(0);
    if (fio.readfile(file,v)==true) {
      int last=atoi(v[1].c_str())+1;
      for (int i=2; i<=last; i++)
        p.push_back( s2p(v[i]) );
      return true;
    }
    return false;
  }

  /*PQR
  // 1234567890123456789012345678901234567890123456789012345678901234567890
  // ATOM     25 H1A  CR      4      23.215  17.973  15.540 -0.017 1.000
  */

  //----------------- IOXTC ----------------------

  ioxtc::ioxtc(float len) {
    prec_xtc = 1000.;
    time_xtc=step_xtc=0;
    setbox(len);
    xd=NULL;
    x_xtc=NULL;
  }

  void ioxtc::setbox(float len) {
    for (short i=0; i<3; i++)
      for (short j=0; j<3; j++)
        xdbox[i][j]=0;
    xdbox[0][0]=0.1*len; // corners of the
    xdbox[1][1]=0.1*len; // rectangular box
    xdbox[2][2]=0.1*len; // in nanometers
  }

  void ioxtc::setbox(double x, double y, double z) {
    for (short i=0; i<3; i++)
      for (short j=0; j<3; j++)
        xdbox[i][j]=0;
    xdbox[0][0]=0.1*x; // corners of the
    xdbox[1][1]=0.1*y; // rectangular box
    xdbox[2][2]=0.1*z; // in nanometers!
  }

  /*!
   * Save all particles in Cuboid to xtc file. Molecules added to the ioxtc::g
   * vector will be made whole (periodic boundaries are temporarily undone). Box
   * dimensions are taken from the Cuboid class and the particles are shifted so
   * that origin is in the corner of the box (Gromacs practice)
   *
   * \param file Name of the output xtc file
   * \param c Cuboid container from which particles and box dimensions are read.
   */
  bool ioxtc::save(string file, Space &c) {
    Geometry::Cuboid* geo = dynamic_cast<Geometry::Cuboid*>(c.geo);
    p=c.p;
    setbox(geo->len.x, geo->len.y, geo->len.z);
    for (auto gi : g) {
      gi->translate( c, -gi->cm );             // b.trial is moved to origo -> whole!
      for (int j = gi->beg; j <= gi->end; j++)
        p[j] = c.trial[j] + gi->cm;            // move back to cm without periodicity
      gi->undo(c);                             // restore to original PBC location
    }
    for (auto &pi : p)
      pi+=geo->len_half;                       // gromacs origo is in the corner of the box
    return save(file, p);                      // while in Cuboid we use the middle
  }

  /*!
   * This will take an arbitrary particle vector and add it
   * to an xtc file. No shifting is done - only modification is conversion
   * from aangstom to nanometers. The box dimensions for the frame must be manually
   * set by the ioxtc::setbox() function before calling this.
   */
  bool ioxtc::save(string file, const p_vec &p) {
    if (xd==NULL)
      xd=xdrfile_open(&file[0], "w");
    if (xd!=NULL) {
      rvec *x = new rvec [p.size()];
      int i=0;
      for (auto &pi : p) {
        x[i][0] = (pi.x ) * 0.1;      // AA->nm
        x[i][1] = (pi.y ) * 0.1;
        x[i][2] = (pi.z ) * 0.1;
        i++;
      }
      write_xtc(xd,p.size(),step_xtc++,time_xtc++,xdbox,x,prec_xtc);
      delete[] x;
      return true;
    }
    return false;
  }

  bool ioxtc::save(string file, p_vec &p, vector<Group> &g) {
    p_vec t;
    for (auto &gi : g)
      for (int j=gi.beg; j<=gi.end; j++)
        t.push_back( p[j] );
    return save(file, t);
  }

  void ioxtc::close() {
    xdrfile_close(xd);
    xd=NULL;
    delete[] x_xtc;
  }

  /*!
   * This will open an xtc file for reading. The number of atoms in each frame
   * is saved and memory for the coordinate array is allocated.
   */
  bool ioxtc::open(string s) {
    if (xd!=NULL)
      close();
    xd = xdrfile_open(&s[0], "r");
    if (xd!=NULL) {
      int rc = read_xtc_natoms(&s[0], &natoms_xtc); // get number of atoms
      if (rc==exdrOK) {
        x_xtc = new rvec [natoms_xtc]; // resize coordinate array
        return true;
      }
    } else
      std::cerr << "# ioxtc error: xtc file could not be opened." << endl;
    return false;
  }

  /*!
   * This will read a single frame from the xtc file (must be open) into
   * a Cuboid container. The box dimensions are retrieved for the frame and transfered
   * to the container. Coordinates are copied into both the particle vector "p" and the
   * "trial" vector. In doing so, positions are converted from nm to angstroms and the
   * coordinate system is shifted so that origin is on the middle of the box. As a safefy
   * measure we do a container collision check to see if all particles are within the Cuboid
   * boundaries.
   *
   * \note The container particle vector *must* match the number of particles in the xtc file. If not
   *       an error message will be issued and the function will abort.
   * \note You may want to transfer the new box size to the pair potential if periodic boundaries are used.
   */
  bool ioxtc::loadnextframe(Space &c) {
    if (xd!=NULL) {
      if (natoms_xtc==(int)c.p.size()) { 
        int rc = read_xtc(xd, natoms_xtc, &step_xtc, &time_xtc, xdbox, x_xtc, &prec_xtc);
        if (rc==0) {
          static double ten=10;
          Geometry::Cuboid* geo = dynamic_cast<Geometry::Cuboid*>(c.geo);
          Point l( xdbox[0][0], xdbox[1][1], xdbox[2][2] );
          geo->setlen(l*ten);
          for (size_t i=0; i<c.p.size(); i++) {
            c.p[i].x = x_xtc[i][0]*ten - geo->len_half.x;     // store pos. in container.                 
            c.p[i].y = x_xtc[i][1]*ten - geo->len_half.y;
            c.p[i].z = x_xtc[i][2]*ten - geo->len_half.z;
            c.trial[i].x=c.p[i].x;
            c.trial[i].y=c.p[i].y;
            c.trial[i].z=c.p[i].z;
            if ( geo->collision(c.p[i]) ) {
              std::cerr << "# ioxtc load error: particle-container collision!" << endl;
              return false;
            }
          } 
          return true;
        }
      } else
        std::cerr << "# ioxtc load error: xtcfile-container particle mismatch!" << endl;
    } else
      std::cerr << "# ioxtc load error: xtc file not available for reading!" << endl;
    return false;
  }

  //----------------- IOQTRAJ ----------------------
  ioqtraj::ioqtraj() {
    append=false;
  }

  p_vec ioqtraj::load(string s) {
    p_vec dummy;
    return dummy;
  }

  bool ioqtraj::save(string file, p_vec &p) {
    io fio;
    std::ostringstream o;
    o.precision(6);
    for (size_t i=0; i<p.size(); i++) {
      o << p[i].charge << " ";
    }
    o << endl;
    if ( append==true )
      return fio.writefile(file, o.str(), std::ios_base::app);
    else
      append=true;
    return fio.writefile(file, o.str(), std::ios_base::out);
  }

  bool ioqtraj::save(string file, p_vec &p, vector<Group> &g) {
    p_vec t;
    for (size_t i=0; i<g.size(); i++)
      for (int j=g[i].beg; j<=g[i].end; j++)
        t.push_back( p[j] );
    return save(file, t);
  }

  xyfile::xyfile(string name) : f(name.c_str()) {
    cnt=0;
  }

  void xyfile::add(double x, double y) {
    f << x << " " << y << std::endl;
  }

  void xyfile::close() {
    f.close();
  }

  FastaSequence::FastaSequence(double harmonic_k, double harmonic_req) : bond(harmonic_k, harmonic_req) {
    map['A']="ALA";
    map['R']="ARG";
    map['N']="ASN";
    map['D']="ASP";
    map['C']="CYS";
    map['E']="GLU";
    map['Q']="GLN";
    map['G']="GLY";
    map['H']="HIS";
    map['I']="ILE";
    map['L']="LEU";
    map['K']="LYS";
    map['M']="MET";
    map['F']="PHE";
    map['P']="PRO";
    map['S']="SER";
    map['T']="THR";
    map['W']="TRP";
    map['Y']="TYR";
    map['V']="VAL";
  }

  p_vec FastaSequence::interpret(string seq) {
    p_vec p;
    particle a;
    for (auto c : seq) {
      if (map.find(c)!=map.end() ) {
        a=atom[ map[c] ];
        p.push_back(a);
      }
    }
    return p;
  }

  Group FastaSequence::insert(string fasta, Space &spc, Energy::ParticleBonds &b) {
    p_vec p = interpret(fasta);
    Group g;
    if (p.size()>0) {
      g.beg=spc.p.size();
      g.end=g.beg-1;
      for (auto &a : p)
        if ( spc.insert(a) )
          g.end++;
      for (int i=g.beg; i<g.end; i++)
        b.add(i, i+1, bond );
    }
    return g;
  }

  Group FastaSequence::include(string file, Space &spc, Energy::ParticleBonds &b) {
    Group g;
    return g;
  }
}  //namespace
