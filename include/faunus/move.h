#ifndef FAUNUS_MOVE_H
#define FAUNUS_MOVE_H

#ifndef SWIG
#include <functional>
#include <memory>
#include <faunus/common.h>
#include <faunus/point.h>
#include <faunus/average.h>
#include <faunus/textio.h>
#include <faunus/geometry.h>
#include <faunus/energy.h>
#include <faunus/textio.h>
#include <faunus/json.h>
#include <faunus/titrate.h>
#include <faunus/species.h>


#ifdef ENABLE_MPI
#include <faunus/mpi.h>
#endif

#endif

namespace Faunus
{

	/**
	 * @brief Monte Carlo move related classes
	 *
	 * All moves are based on `Movebase` and most end-users
	 * will probably want to start with `Propagator` which
	 * collects all moves and allows for control via input
	 * JSON files.
	 */
	namespace Move
	{

		template<typename Tkey=std::string>
			class AcceptanceMap
			{
				private:
					typedef std::map<Tkey, Average<double> > map_type;
					map_type accmap;   //!< move acceptance ratio
					map_type sqrmap;   //!< mean square displacement map
				public:
					void accept( Tkey k, double msq )
					{
						accmap[k] += 1;
						sqrmap[k] += msq;
					}

					void reject( Tkey k )
					{
						accmap[k] += 0;
					}

					string info( char l = 10 )
					{
						using namespace textio;
						std::ostringstream o;
						o << indent(SUB) << "Move Statistics:" << endl
							<< indent(SUBSUB) << std::left << setw(20) << "Id"
							<< setw(l + 1) << "Acc. " + percent
							<< setw(l) << "Nmoves"
							<< setw(l + 9) << rootof + bracket("msq" + squared) + "/" + angstrom << endl;
						for ( auto m : accmap )
						{
							string id = m.first;
							o << indent(SUBSUB) << std::left << setw(20) << id;
							o.precision(3);
							o << setw(l) << accmap[id].avg() * 100
								<< setw(l) << accmap[id].cnt
								<< setw(l) << sqrt(sqrmap[id].avg()) << endl;
						}
						return o.str();
					}

					void _test( UnitTest &t, const string &prefix )
					{
						for ( auto &m : accmap )
						{
							std::ostringstream o;
							o << m.first;
							t(prefix + "_Acceptance" + o.str(), m.second.avg());
						}
					}
			};

		/**
		 * @brief Add polarisation step to an arbitrary move
		 *
		 * This class will modify any MC move to account for polarization
		 * using an iterative procedure.
		 * An electric field calculation is inserted
		 * after the original trial move whereafter it will iteratively
		 * calculate induced dipole moments on all particles.
		 * The energy change function will evaluate the *total*
		 * system energy as all dipoles in the system may have changed.
		 * This is thus an expensive computation and is best used with
		 * MC moves that propagate  all particles.
		 *
		 * Updating induced moments is an iterative N*N operation and
		 * very inefficient for MC moves that update only a subset of the system.
		 * In liquid systems that propagate only slowly as a function of MC steps
		 * one may attempt to update induced dipoles less frequently at the
		 * expense of accuracy.
		 * For repeating moves -- i.e. molecular translate/rotate or atomic
		 * translation -- polarisation is updated only after all moves have
		 * been carried out.
		 *
		 * @note Will currently not work for Grand Caninical moves
		 */
		template<class Tmove>
			class PolarizeMove : public Tmove
		{
			private:
				using Tmove::spc;
				using Tmove::pot;
				int Ntrials;                    // Number of repeats within move
				int max_iter;                   // max numbr of iterations
				double threshold;          // threshold for iteration
				bool updateDip;                 // true if ind. dipoles should be updated
				Eigen::MatrixXd field;      // field on each particle
				Average<int> numIter;           // average number of iterations per move

				/**
				 *  @brief Updates dipole moment w. permanent plus induced dipole moment
				 *  @param pot Hamiltonian
				 *  @param p Particles to update
				 */
				template<typename Tenergy, typename Tparticles>
					void induceDipoles( Tenergy &pot, Tparticles &p )
					{

						int cnt = 0;
						Eigen::VectorXd mu_err_norm((int) p.size());

						do
						{
							cnt++;
							mu_err_norm.setZero();
							field.setZero();
							pot.field(p, field);
							for ( size_t i = 0; i < p.size(); i++ )
							{
								Point E = field.col(i);                  // field on i
								Point mu_trial = p[i].alpha() * E + p[i].mup();// new tot. dipole
								Point mu_err = mu_trial - p[i].mu() * p[i].muscalar();// mu difference
								mu_err_norm[i] = mu_err.norm();          // norm of previous row
								p[i].muscalar() = mu_trial.norm();         // update dip scalar in particle
								if ( p[i].muscalar() > 1e-6 )
									p[i].mu() = mu_trial / p[i].muscalar();      // update article dip.
							}
							if ( cnt > max_iter )
								throw std::runtime_error("Field induction reached maximum number of iterations.");

						}
						while ( mu_err_norm.maxCoeff() > threshold ); // is threshold OK?

						numIter += cnt; // average number of iterations
					}

				void _trialMove() override
				{
					Tmove::_trialMove();

					Ntrials++;
					int updateAt = 1;  // default: dipoles are always updated
					if ( !Tmove::mollist.empty())
						updateAt = Tmove::mollist[Tmove::currentMolId].repeat;
					else
						Ntrials = 1;      // in case move(n) is called w. n>1

					updateDip = (Ntrials == updateAt);

					if ( updateDip )
					{
						field.resize(3, spc->trial.size());
						induceDipoles(*pot, spc->trial);
					}
				}

				double _energyChange() override
				{
					if ( updateDip )
						return Energy::systemEnergy(*spc, *pot, spc->trial)
							- Energy::systemEnergy(*spc, *pot, spc->p);
					else
						return Tmove::_energyChange();
				}

				void _rejectMove() override
				{
					Tmove::_rejectMove();
					if ( updateDip )
						Tmove::spc->trial = Tmove::spc->p;
				}

				void _acceptMove() override
				{
					Tmove::_acceptMove();
					if ( updateDip )
						Tmove::spc->p = Tmove::spc->trial;
				}

				string _info() override
				{
					std::ostringstream o;
					using namespace textio;
					o << pad(SUB, Tmove::w, "Polarisation updates") << numIter.cnt << "\n"
						<< pad(SUB, Tmove::w, "Polarisation threshold") << threshold << "\n"
						<< pad(SUB, Tmove::w, "Polarisation iterations") << numIter.avg()
						<< " (max. " << max_iter << ")" << "\n"
						<< Tmove::_info();
					return o.str();
				}

			public:

				double getThreshold() const { return threshold; }
				int getMaxIterations() const { return max_iter; }

				template<class Tspace>
					PolarizeMove( Tmjson &in, Energy::Energybase<Tspace> &e, Tspace &s ) :
						Tmove(in, e, s)
			{
				threshold = in.value("pol_threshold", 0.001);
				max_iter = in.value("max_iterations", 40);
			}

				template<class Tspace>
					PolarizeMove( Energy::Energybase<Tspace> &e, Tspace &s, Tmjson &j ) :
						Tmove(e, s, j)
			{
				threshold = j.value("pol_threshold", 0.001);
				max_iter = j.value("max_iterations", 40);
			}

				//PolarizeMove( const Tmove &m ) : max_iter(40), threshold(0.001), Tmove(m) {};

				double move( int n ) override
				{
					Ntrials = 0;
					return Tmove::move(n);
				}
		};

		/**
		 * @brief Base class for Monte Carlo moves
		 *
		 * The is a base class that handles Monte Carlo moves and derived classes
		 * are required to implement the following pure virtual (and private)
		 * functions:
		 *
		 * - `_trialMove()`
		 * - `_energyChange()`
		 * - `_acceptMove()`
		 * - `_rejectMove()`
		 * - `_info()`
		 *
		 * These functions should be pretty self-explanatory and are - via wrapper
		 * functions - called by move(). It is important that the _energyChange() function
		 * returns the full energy associated with the move. For example, for NPT
		 * moves the pV term should be included and so on. Try not to override
		 * the move() function as this should be generic to all MC moves.
		 *
		 * @date Lund, 2007-2011
		 */
		template<class Tspace>
			class Movebase
			{
				private:
					unsigned long int cnt_accepted;  //!< number of accepted moves
					double dusum;                    //!< Sum of all energy changes

					virtual void _test( UnitTest & );   //!< Unit testing
					virtual void _trialMove()=0;     //!< Do a trial move
					virtual void _acceptMove()=0;    //!< Accept move and config
					virtual void _rejectMove()=0;    //!< Reject move and config
					virtual double _energyChange()=0;//!< Energy change of move (kT)

					void acceptMove();               //!< Accept move (wrapper)
					void rejectMove();               //!< Reject move (wrapper)
					double energyChange();           //!< Energy (wrapper)
					bool metropolis( const double & ) const;//!< Metropolis criteria

					TimeRelativeOfTotal<std::chrono::microseconds> timer;

					/** @brief Information as JSON object */
					virtual Tmjson _json() { return Tmjson(); }

				protected:
					virtual string _info()=0;        //!< info for derived moves
					void trialMove();                //!< Do a trial move (wrapper)
					Energy::Energybase<Tspace> *pot; //!< Pointer to energy functions
					Space<typename Tspace::GeometryType, typename Tspace::ParticleType> *spc; //!< Pointer to Space
					string title;                    //!< Title of move (mandatory!)
					string cite;                     //!< Reference, url, DOI etc.
					char w;                          //!< info text width. Adjust this in constructor if needed.
					unsigned long int cnt;           //!< total number of trial moves
					virtual bool run();              //!< Runfraction test
					typename Tspace::Change change;  //!< Object describing changes made to Space

					bool useAlternativeReturnEnergy;   //!< Return a different energy than returned by _energyChange(). [false]
					double alternateReturnEnergy;    //!< Alternative return energy

					struct MolListData
					{
						double prob;  // probability of performing a move
						bool perAtom; // repeat move for each molecule?
						bool perMol;  // repeat move for atom in molecules?
						int repeat;   // total number of repeats
						unsigned long Nattempts;  // # of attempted moves
						unsigned long Naccepted;  // # of accepted moves
						Point dir;    // translational move directions
						double dp1;   // displacement parameter 1
						double dp2;   // displacement parameter 2

						MolListData() : prob(1.0), perAtom(false), perMol(false),
						repeat(1), Nattempts(0), Naccepted(0), dir(1, 1, 1), dp1(0), dp2(0) {}

						MolListData( Tmjson &j )
						{
							*this = MolListData();
							prob = j.value("prob", 1.0);
							perMol = j.value("permol", false);
							perAtom = j.value("peratom", false);
							dir << (j["dir"] | std::string("1 1 1"));
						}
					};

					std::map<int, MolListData> mollist;    //!< Move acts on these molecule id's

					/**
					 * @brief Iterate over json object where each key is a molecule
					 *        name and the value is read as `MolListData`.
					 */
					void fillMolList( Tmjson &j )
					{
						for ( auto it = j.begin(); it != j.end(); ++it )
						{  // iterate over molecules
							auto mol = spc->molList().find(it.key()); // is molecule defined?
							if ( mol != spc->molList().end())
								addMol(mol->id, MolListData(it.value()));
#ifndef NDEBUG
							else
								std::cerr << title << ": unknown molecule '" << it.key() << "' was not added.\n";
#endif
						}
					}

					/** @brief Internal, deterministic random number generator, independent of global */
					static RandomTwister<> &_slump()
					{
						static RandomTwister<> r;
						return r;
					}

				public:
					Movebase( Energy::Energybase<Tspace> &, Tspace & );//!< Constructor
					virtual ~Movebase();
					double
						runfraction;                //!< Fraction of times calling move() should result in an actual move. 0=never, 1=always.
					virtual double move( int= 1 );        //!< Attempt `n` moves and return energy change (kT)
					string info();                     //!< Returns information string
					void test( UnitTest & );              //!< Perform unit test
					double getAcceptance() const;      //!< Get acceptance [0:1]

					void addMol( int, const MolListData &d = MolListData()); //!< Specify molecule id to act upon
					Group *randomMol();
					int randomMolId();                 //!< Random mol id from mollist
					int currentMolId;                  //!< Current molid to act upon

					Tmjson json()
					{                    //!< Information as JSON object
						Tmjson j;
						if ( cnt > 0 )
						{
							j[title] = {
								{"trials", cnt},
								{"acceptance", getAcceptance()},
								{"runfraction", runfraction},
								{"relative time", timer.result()}
							};
							j = merge(j, _json());
						}
						return j;
					}

#ifdef ENABLE_MPI
					Faunus::MPI::MPIController* mpiPtr;
#endif
			};

		/**
		 * @brief Constructor
		 * @param e Energy class
		 * @param s Space
		 */
		template<class Tspace>
			Movebase<Tspace>::Movebase( Energy::Energybase<Tspace> &e, Tspace &s )
			{
				e.setSpace(s);
				pot = &e;
				spc = &s;
				cnt = cnt_accepted = 0;
				dusum = 0;
				w = 30;
				runfraction = 1;
				useAlternativeReturnEnergy = false; //this has no influence on metropolis sampling!
				change.clear();
#ifdef ENABLE_MPI
				mpiPtr=nullptr;
#endif
			}

		template<class Tspace>
			Movebase<Tspace>::~Movebase() {}

		template<class Tspace>
			void Movebase<Tspace>::addMol( int molid, const MolListData &d )
			{
				mollist[molid] = d;
			}

		template<class Tspace>
			int Movebase<Tspace>::randomMolId()
			{
				if ( !mollist.empty())
				{
					auto it = _slump().element(mollist.begin(), mollist.end());
					if ( it != mollist.end())
					{
						it->second.repeat = 1;
						if ( it->second.perMol )
							it->second.repeat *= spc->numMolecules(it->first);
						if ( it->second.perAtom )
							it->second.repeat *= spc->findMolecules(it->first).front()->size();
						return it->first;
					}
				}
				return -1;
			}

		/**
		 * Returns pointer to a random group matching a molecule id
		 * in `mollist`
		 */
		template<class Tspace>
			Group *Movebase<Tspace>::randomMol()
			{
				Group *gPtr = nullptr;
				if ( !mollist.empty())
				{
					auto it = _slump().element(mollist.begin(), mollist.end());
					auto g = spc->findMolecules(it->first); // vector of group pointers
					if ( !g.empty())
						gPtr = *_slump().element(g.begin(), g.end());
				}
				return gPtr;
			}

		template<class Tspace>
			void Movebase<Tspace>::trialMove()
			{
				assert(change.empty() && "Change object is not empty!");
				if ( cnt == 0 )
					for ( auto i : spc->groupList())
						i->setMassCenter(*spc);
				cnt++;
				_trialMove();
			}

		template<class Tspace>
			void Movebase<Tspace>::acceptMove()
			{
				cnt_accepted++;
				_acceptMove();
			}

		template<class Tspace>
			void Movebase<Tspace>::rejectMove()
			{
				_rejectMove();
			}

		/** @return Energy change in units of kT */
		template<class Tspace>
			double Movebase<Tspace>::energyChange()
			{
				double du = _energyChange();
				if ( std::isnan(du))
					std::cerr << "Warning: energy change from move returns not-a-number (NaN)" << endl;
				return du;
			}

		/**
		 * This function performs trial move and accept/reject using
		 * the Metropolis criteria.
		 * It carries out the following `n` times:
		 *
		 * - Perform a trial move with `_trialMove()`
		 * - Calulate the energy change, \f$\beta\Delta U\f$ with `_energyChange()`
		 * - Accept with probability \f$ \min(1,e^{-\beta\Delta U}) \f$
		 * - Call either `_acceptMove()` or `_rejectMove()`
		 *
		 * @note Do not override this function in derived classes.
		 * @param n Perform move `n` times (default=1)
		 *
		 * [More info](http://dx.doi.org/10.1063/1.1699114)
		 */
		template<class Tspace>
			double Movebase<Tspace>::move( int n )
			{
				timer.start();
				double utot = 0;

				if ( !mollist.empty())
				{
					currentMolId = randomMolId();
					n = mollist[currentMolId].repeat;
					runfraction = mollist[currentMolId].prob;
				}

				if ( run())
				{
					bool acceptance = true;
					while ( n-- > 0 )
					{
						trialMove();
						pot->updateChange(change);

						double du = energyChange();
						acceptance = metropolis(du); // true or false?
						if ( !acceptance )
							rejectMove();
						else
						{
							acceptMove();
							if ( useAlternativeReturnEnergy )
								du = alternateReturnEnergy;
							dusum += du;
							utot += du;
						}
						utot += pot->update(acceptance);
						change.clear();
					}
				}
				assert(spc->p == spc->trial && "Trial particle vector out of sync!");
				timer.stop();
				return utot;
			}

		/**
		 * @param du Energy change for MC move (kT)
		 * @return True if move should be accepted; false if not.
		 * @note
		 * One could put in `if (du>0)` before the first line, but
		 * certain MPI communications require the random number
		 * generator to be in sync, i.e. each rank must call
		 * `slump()` equal number of times, independent of
		 * dU.
		 */
		template<class Tspace>
			bool Movebase<Tspace>::metropolis( const double &du ) const
			{
				if ( slump() > std::exp(-du)) // core of MC!
					return false;
				return true;
			}

		template<class Tspace>
			bool Movebase<Tspace>::run()
			{
				if ( _slump()() < runfraction )
					return true;
				return false;
			}

		template<class Tspace>
			void Movebase<Tspace>::test( UnitTest &t )
			{
				if ( runfraction < 1e-6 || cnt == 0 )
					return;
				t(textio::trim(title) + "_acceptance", double(cnt_accepted) / cnt * 100);
				_test(t);
			}

		template<class Tspace>
			void Movebase<Tspace>::_test( UnitTest & )
			{
			}

		template<class Tspace>
			double Movebase<Tspace>::getAcceptance() const
			{
				if ( cnt > 0 )
					return double(cnt_accepted) / cnt;
				return 0;
			}

		/**
		 * This will return a formatted multi-line information string about the move and
		 * will as a minimum contain:
		 *
		 * - Name of move
		 * - Runfraction
		 * - Number of times the move has been called
		 * - Acceptance
		 * - Total energy change
		 *
		 * Typically, additional information will be provided as well.
		 *
		 * @note Do not override in derived classes - use _info().
		 */
		template<class Tspace>
			string Movebase<Tspace>::info()
			{
				using namespace textio;
				assert(!title.empty() && "Markov Moves must have a title");
				std::ostringstream o;
				if ( runfraction < 1e-10 )
					return o.str();
				o << header("Markov Move: " + title);
				if ( !cite.empty())
					o << pad(SUB, w, "More information:") << cite << endl;
				if ( cnt > 0 )
					o << pad(SUB, w, "Number of trials") << cnt << endl
						<< pad(SUB, w, "Relative time consumption") << timer.result() << endl
						<< pad(SUB, w, "Acceptance") << getAcceptance() * 100 << percent << endl
						<< pad(SUB, w, "Runfraction") << runfraction * 100 << percent << endl
						<< pad(SUB, w, "Total energy change") << dusum << kT << endl;
				o << _info();
				return o.str();
			}

		/**
		 * @brief Generate new configurations by looping through XTC trajectory
		 *
		 * This move will load frames from a trajectory file and with this
		 * replace particle positions in the system. No energy is evaluated
		 * and energyChange will always return 0.
		 *
		 * If the trajectory is saved with molecules that extend beyond
		 * the box boundaries, the PBC boundary control should be set
		 * to true.
		 *
		 *  Keyword  | Description
		 *  -------- | ---------------
		 *  `file`   | Trajectory file to load (.xtc)
		 *  `trump`  | Enforce (PBC) boundary control (default: false)
		 *
		 * Notes:
		 *
		 *   - Geometry must be derived from `Geometry::Cuboid`.
		 *   - Number of particle in Space must match that of the trajectory
		 *   - Particles must not overlap with geometry boundaries
		 *   - if the above is violated an exception is thrown
		 *
		 * @date January 2017
		 * @warning Untested
		 */
		template<class Tspace, class base=Movebase<Tspace>>
			class TrajectoryMove : public base
		{
			FormatXTC xtc;
			bool _continue;
			int framecnt;
			string file;
			bool applyPBC; // true if PBC should be applied to loaded frames

			void _acceptMove() override {};
			void _rejectMove() override {};
			string _info() override { return string(); };
			double _energyChange() override { return 0; };

			Tmjson _json() override
			{
				Tmjson js;
				if ( base::cnt > 0 )
				{
					auto &j = js[base::title];
					j = {
						{ "file", file },
						{ "boundary control", applyPBC },
						{ "frames loaded", framecnt}
					};
				}
				return js;
			}

			void _trialMove() override
			{
				if (_continue)
					_continue = xtc.loadnextframe( *base::spc, true, applyPBC );
				if (_continue)
					framecnt++;
			}

			public:

			TrajectoryMove( Energy::Energybase<Tspace> &e, Tspace &s, Tmjson &j )
				: Movebase<Tspace>(e, s), xtc(1), _continue(true), framecnt(0)
			{
				base::title = "XTC Trajectory Move";
				file = j.at("file");
				applyPBC = j.value("trump", false);
				if ( xtc.open(file) == false)
					throw std::runtime_error(base::title + ": xtc file " + file + " cannot be loaded");
			}

			bool eof() { return _continue; } //!< True if all frames have been loaded
		};

		/**
		 * @brief Translation of atomic particles
		 *
		 * This Markov move can work in two modes:
		 * - Move a single particle in space set by setParticle()
		 * - Move single particles randomly selected in a Group set by setGroup().
		 *
		 * The move directions can be controlled with the dir vector - for instance if you wish
		 * to translate only in the `z` direction, set `dir.x()=dir.y()=0`.
		 *
		 * @date Lund, 2011
		 */
		template<class Tspace>
			class AtomicTranslation : public Movebase<Tspace>
		{
			private:
				typedef Movebase<Tspace> base;
				typedef std::map<short, Average<double> > map_type;
				bool run() override; //!< Runfraction test
				Tmjson _json() override;

			protected:
				string _info() override;
				void _acceptMove() override;
				void _rejectMove() override;
				double _energyChange() override;
				void _trialMove() override;
				using base::spc;
				map_type accmap; //!< Single particle acceptance map
				map_type sqrmap; //!< Single particle mean square displacement map

				int iparticle;   //!< Select single particle to move (-1 if none, default)
				Group *igroup;   //!< Group pointer in which particles are moved randomly (NULL if none, default)
				double genericdp;//!< Generic atom displacement parameter - ignores individual dps
				Average<unsigned long long int> gsize; //!< Average size of igroup;

			public:

				AtomicTranslation( Energy::Energybase<Tspace> &, Tspace &, Tmjson & );

				void setGenericDisplacement( double ); //!< Set single displacement for all atoms

				Point dir;             //!< Translation directions (default: x=y=z=1)
		};

		/**
		 * @brief Constructor
		 *
		 * The json entry is read on a per-molecule basis, each with
		 * the following keywords,
		 *
		 * Value                | Description
		 * :------------------- | :-------------------------------------------------------------
		 * `dir`                | Move directions (default: "1 1 1" = xyz)
		 * `peratom`            | Repeat move for each atom in molecule (default: false)
		 * `permol`             | Repeat move for each molecule in system (default: false)
		 * `prob`               | Probability of performing the move (default: 1)
		 *
		 * Example:
		 *
		 *     {
		 *       "salt" : { "dir":"1 1 0", "peratom":true }
		 *     }
		 *
		 * Atomic displacement parameters are read from `Faunus::AtomData`.
		 */
		template<class Tspace>
			AtomicTranslation<Tspace>::AtomicTranslation(
					Energy::Energybase<Tspace> &e, Tspace &s, Tmjson &j ) : Movebase<Tspace>(e, s)
			{
				base::title = "Single Particle Translation";
				iparticle = -1;
				igroup = nullptr;
				dir = {1, 1, 1};
				genericdp = 0;
				base::fillMolList(j);
			}

		/**
		 * The generic displacement parameter will be used only if the specific
		 * atomic dp is zero.
		 */
		template<class Tspace>
			void AtomicTranslation<Tspace>::setGenericDisplacement( double dp )
			{
				genericdp = dp;
			}

		template<class Tspace>
			bool AtomicTranslation<Tspace>::run()
			{
				if ( !this->mollist.empty() )
					if ( spc->findMolecules(this->currentMolId).empty() )
						return false;
				if ( igroup != nullptr )
					if ( igroup->empty())
						return false;
				return base::run();
			}

		template<class Tspace>
			void AtomicTranslation<Tspace>::_trialMove()
			{
				if ( !this->mollist.empty())
				{
					auto gvec = spc->findMolecules(this->currentMolId);
					assert(!gvec.empty());
					igroup = *slump.element(gvec.begin(), gvec.end());
					assert(!igroup->empty());
					dir = this->mollist[this->currentMolId].dir;
				}

				if ( igroup != nullptr )
				{
					iparticle = igroup->random();
					gsize += igroup->size();
				}
				if ( iparticle > -1 )
				{
					double dp = atom[spc->p.at(iparticle).id].dp;
					if ( dp < 1e-6 )
						dp = genericdp;
					assert(iparticle < (int) spc->p.size()
							&& "Trial particle out of range");
					Point t = dir * dp;
					t.x() *= slump() - 0.5;
					t.y() *= slump() - 0.5;
					t.z() *= slump() - 0.5;
					spc->trial[iparticle].translate(spc->geo, t);

					// make sure trial mass center is updated for molecular groups
					// (certain energy functions may rely on up-to-date mass centra)
					auto gi = spc->findGroup(iparticle);
					assert(gi != nullptr);
					assert((gi->cm - gi->cm_trial).squaredNorm() < 1e-6);
					if ( gi->isMolecular())
						gi->cm_trial = Geometry::massCenter(spc->geo, spc->trial, *gi);

#ifndef NDEBUG
					// are untouched particles in group synched?
					for ( auto j : *gi )
						if ( j != iparticle )
							assert((base::spc->p[j] - base::spc->trial[j]).squaredNorm() < 1e-6);
#endif
				}
				base::change.mvGroup[spc->findIndex(igroup)].push_back(iparticle);
			}

		template<class Tspace>
			void AtomicTranslation<Tspace>::_acceptMove()
			{
				double r2 = spc->geo.sqdist(spc->p[iparticle], spc->trial[iparticle]);
				sqrmap[spc->p[iparticle].id] += r2;
				accmap[spc->p[iparticle].id] += 1;
				spc->p[iparticle] = spc->trial[iparticle];
				auto gi = spc->findGroup(iparticle);
				assert(gi != nullptr);
				if ( gi->isMolecular())
					gi->cm = gi->cm_trial;
			}

		template<class Tspace>
			void AtomicTranslation<Tspace>::_rejectMove()
			{
				spc->trial[iparticle] = spc->p[iparticle];
				sqrmap[spc->p[iparticle].id] += 0;
				accmap[spc->p[iparticle].id] += 0;
				auto gi = spc->findGroup(iparticle);
				assert(gi != nullptr);
				if ( gi->isMolecular())
					gi->cm_trial = gi->cm;
			}

		template<class Tspace>
			double AtomicTranslation<Tspace>::_energyChange()
			{
				if ( iparticle > -1 )
				{
					assert(spc->geo.collision(spc->p[iparticle], spc->p[iparticle].radius) == false
							&& "An untouched particle collides with simulation container.");
					return Energy::energyChange(*spc, *base::pot, base::change);
				}
				return 0;
			}

		template<class Tspace>
			string AtomicTranslation<Tspace>::_info()
			{
				using namespace textio;
				std::ostringstream o;
				if ( gsize.cnt > 0 )
					o << pad(SUB, base::w, "Average moves/particle")
						<< base::cnt / gsize.avg() << endl;
				o << pad(SUB, base::w, "Displacement vector")
					<< dir.transpose() << endl;
				if ( genericdp > 1e-6 )
					o << pad(SUB, base::w, "Generic displacement")
						<< genericdp << _angstrom << endl;
				if ( base::cnt > 0 )
				{
					char l = 12;
					o << endl
						<< indent(SUB) << "Individual particle movement:" << endl << endl
						<< indent(SUBSUB) << std::left << string(7, ' ')
						<< setw(l - 6) << "dp"
						<< setw(l + 1) << "Acc. " + percent
						<< setw(l) << "Nmoves"
						<< setw(l + 7) << bracket("r" + squared) + "/" + angstrom + squared
						<< rootof + bracket("r" + squared) + "/" + angstrom << endl;
					for ( auto m : sqrmap )
					{
						auto id = m.first;
						o << indent(SUBSUB) << std::left << setw(7) << atom[id].name
							<< setw(l - 6) << ((atom[id].dp < 1e-6) ? genericdp : atom[id].dp);
						o.precision(3);
						o << setw(l) << accmap[id].avg() * 100
							<< setw(l) << accmap[id].cnt
							<< setw(l) << sqrmap[id].avg()
							<< setw(l) << sqrt(sqrmap[id].avg()) << endl;
					}
				}
				return o.str();
			}

		/** @brief Create json object with move details */
		template<class Tspace>
			Tmjson AtomicTranslation<Tspace>::_json()
			{
				Tmjson js;
				if ( base::cnt > 0 )
				{
					auto &j = js[base::title];
					j = {
						{"moves/particle", base::cnt / gsize.avg()},
						{"dir", vector<double>( dir ) },
						{"genericdp", genericdp}
					};
					for ( auto m : sqrmap )
					{ // loop over particle id
						int id = m.first;
						double dp = (atom[id].dp < 1e-6) ? genericdp : atom[id].dp;
						j["atoms"][atom[id].name] = {
							{"dp", dp},
							{"acceptance", accmap[id].avg() * 100},
							{"mean displacement", sqrt(sqrmap[id].avg())}
						};
					}
				}
				return js;
			}

		/**
		 * @brief Rotate single particles
		 *
		 * This move works in the same way as AtomicTranslation but does
		 * rotations of non-isotropic particles instead of translation. This move
		 * has no effect on isotropic particles such as Faunus::PointParticle.
		 */
		template<class Tspace>
			class AtomicRotation : public AtomicTranslation<Tspace>
		{
			protected:
				typedef AtomicTranslation<Tspace> base;
				using base::spc;
				using base::iparticle;
				using base::igroup;
				using base::w;
				using base::gsize;
				using base::genericdp;
				using base::accmap;
				using base::sqrmap;
				Geometry::QuaternionRotate rot;
				string _info();
				void _trialMove();
				void _acceptMove();
				void _rejectMove();
				double dprot;      //!< Temporary storage for current angle

			public:
				AtomicRotation( Energy::Energybase<Tspace> &, Tspace &, Tmjson & );
		};

		template<class Tspace>
			AtomicRotation<Tspace>::AtomicRotation(
					Energy::Energybase<Tspace> &e, Tspace &s, Tmjson &j ) : base(e, s, j)
			{
				base::title = "Single Particle Rotation";
			}

		template<class Tspace>
			void AtomicRotation<Tspace>::_trialMove()
			{
				if ( !this->mollist.empty())
				{
					igroup = spc->randomMol(this->currentMolId);
					if ( igroup != nullptr )
					{
						iparticle = igroup->random();
						gsize += igroup->size();
					}
					else
						return;
				}
				else
					return;

				if ( iparticle > -1 )
				{
					assert(iparticle < (int) spc->p.size() && "Trial particle out of range");
					dprot = atom[spc->p[iparticle].id].dprot;
					if ( dprot < 1e-6 )
						dprot = base::genericdp;

					Point u;
					u.ranunit(slump);
					rot.setAxis(spc->geo, Point(0, 0, 0), u, dprot * slump.half());
					spc->trial[iparticle].rotate(rot);
				}
				base::change.mvGroup[spc->findIndex(igroup)].push_back(iparticle);
			}

		template<class Tspace>
			void AtomicRotation<Tspace>::_acceptMove()
			{
				sqrmap[spc->p[iparticle].id] += pow(dprot * 180 / pc::pi, 2);
				accmap[spc->p[iparticle].id] += 1;
				spc->p[iparticle] = spc->trial[iparticle];
			}

		template<class Tspace>
			void AtomicRotation<Tspace>::_rejectMove()
			{
				spc->trial[iparticle] = spc->p[iparticle];
				sqrmap[spc->p[iparticle].id] += 0;
				accmap[spc->p[iparticle].id] += 0;
			}

		template<class Tspace>
			string AtomicRotation<Tspace>::_info()
			{
				using namespace textio;
				std::ostringstream o;
				if ( gsize.cnt > 0 )
					o << pad(SUB, w, "Average moves/particle") << base::cnt / gsize.avg() << endl;
				if ( base::genericdp > 1e-6 )
					o << pad(SUB, w, "Generic displacement") << genericdp
						<< _angstrom << endl;
				if ( base::cnt > 0 )
				{
					char l = 12;
					o << endl
						<< indent(SUB) << "Individual particle rotation:" << endl << endl
						<< indent(SUBSUB) << std::left << string(7, ' ')
						<< setw(l - 6) << "dp"
						<< setw(l + 1) << "Acc. " + percent
						<< setw(l + 7) << bracket("d" + theta + squared) + "/" + degrees
						<< rootof + bracket("d" + theta + squared) + "/" + degrees << endl;
					for ( auto m : sqrmap )
					{
						auto id = m.first;
						o << indent(SUBSUB) << std::left << setw(7) << atom[id].name
							<< setw(l - 6) << ((atom[id].dprot < 1e-6) ? genericdp : atom[id].dprot * 180 / pc::pi);
						o.precision(3);
						o << setw(l) << accmap[id].avg() * 100
							<< setw(l) << sqrmap[id].avg()
							<< setw(l) << sqrt(sqrmap[id].avg()) << endl;
					}
				}
				return o.str();
			}

		/**
		 * @brief Translate single particles
		 *
		 * This move works in the same way as AtomicTranslation but does
		 * translations on a 2D hypersphere-surface.
		 */
		template<class Tspace>
			class AtomicTranslation2D : public AtomicTranslation<Tspace> {
				protected:
					typedef AtomicTranslation<Tspace> base;
					using base::spc;
					using base::iparticle;
					using base::igroup;
					using base::w;
					using base::gsize;
					using base::genericdp;
					using base::accmap;
					using base::sqrmap;
					Geometry::QuaternionRotate rot;
					string _info();
					void _trialMove();
					void _acceptMove();
					void _rejectMove();
					double dp;      //!< Temporary storage for current angle
					double radius;

				public:
					AtomicTranslation2D(Energy::Energybase<Tspace> &, Tspace &,Tmjson &);
			};

		template<class Tspace>
			AtomicTranslation2D<Tspace>::AtomicTranslation2D(
					Energy::Energybase<Tspace> &e,
					Tspace &s,
					Tmjson &j) : base(e, s, j) {
				base::title="Single Particle Translation 2D sphere";
				radius = s.geo.getRadius();
				assert(radius > 0 && "Radius has to be larger than zero!");
			}
		template<class Tspace>
			void AtomicTranslation2D<Tspace>::_trialMove() {
				if ( ! this->mollist.empty() ) {
					igroup = spc->randomMol( this->currentMolId );
					if ( igroup != nullptr ) {
						iparticle = igroup->random();
						gsize += igroup->size();
					} else return;
				} else return;

				if (iparticle>-1) {
					assert( iparticle<(int)spc->p.size() && "Trial particle out of range");
					dp = atom[spc->p[iparticle].id ].dp;
					if (dp<1e-6)
						dp = base::genericdp;

					Point rtp = spc->trial[iparticle].xyz2rtp(); // Get the spherical coordinates of the particle
					double slump_theta = dp*(slump()-0.5);  // Get random theta-move
					double slump_phi = dp*(slump()-0.5);   // Get random phi-move

					double scalefactor_theta = radius*sin(rtp.z()); // Scale-factor for theta
					double scalefactor_phi = radius;                // Scale-factor for phi

					Point theta_dir = Point(-sin(rtp.y()),cos(rtp.y()),0);    // Unit-vector in theta-direction
					Point phi_dir = Point(cos(rtp.y())*cos(rtp.z()),sin(rtp.y())*cos(rtp.z()),-sin(rtp.z()));  // Unit-vector in phi-direction
					Point xyz = spc->trial[iparticle] + scalefactor_theta*theta_dir*slump_theta + scalefactor_phi*phi_dir*slump_phi; // New position
					spc->trial[iparticle] = radius*xyz/xyz.norm(); // Convert to cartesian coordinates

					assert( fabs((spc->trial[iparticle].norm() - radius)/radius) < 1e-9 && "Trial particle does not lie on the sphere surface!");
				}
				base::change.mvGroup[spc->findIndex(igroup)].push_back(iparticle);
			}

		template<class Tspace>
			void AtomicTranslation2D<Tspace>::_acceptMove() {
				sqrmap[ spc->p[iparticle].id ] += pow(dp*180/pc::pi, 2);
				accmap[ spc->p[iparticle].id ] += 1;
				spc->p[iparticle] = spc->trial[iparticle];
			}

		template<class Tspace>
			void AtomicTranslation2D<Tspace>::_rejectMove() {
				spc->trial[iparticle] = spc->p[iparticle];
				sqrmap[ spc->p[iparticle].id ] += 0;
				accmap[ spc->p[iparticle].id ] += 0;
			}

		template<class Tspace>
			string AtomicTranslation2D<Tspace>::_info() {
				using namespace textio;
				std::ostringstream o;
				o << pad(SUB,w,"Radius") << radius << endl;
				if (gsize.cnt>0)
					o << pad(SUB,w,"Average moves/particle") << base::cnt / gsize.avg() << endl;
				if (base::genericdp>1e-6)
					o << pad(SUB,w,"Generic displacement") << genericdp
						<< _angstrom << endl;
				if (base::cnt>0) {
					char l=12;
					o << endl
						<< indent(SUB) << "Individual particle rotation:" << endl << endl
						<< indent(SUBSUB) << std::left << string(7,' ')
						<< setw(l-6) << "dp"
						<< setw(l+1) << "Acc. "+percent
						<< setw(l+7) << bracket("d"+theta+squared)+"/"+degrees
						<< rootof+bracket("d"+theta+squared)+"/"+degrees << endl;
					for (auto m : sqrmap) {
						auto id=m.first;
						o << indent(SUBSUB) << std::left << setw(7) << atom[id].name
							<< setw(l-6) << ( (atom[id].dp<1e-6) ? genericdp : atom[id].dp);
						o.precision(3);
						o << setw(l) << accmap[id].avg()*100
							<< setw(l) << sqrmap[id].avg()
							<< setw(l) << sqrt(sqrmap[id].avg()) << endl;
					}
				}
				return o.str();
			} 

		/**
		 * @brief Combined rotation and rotation of groups
		 *
		 * This will translate and rotate groups and collect averages based on group name.
		 * See constructor for usage.
		 */
		template<class Tspace>
			class TranslateRotate : public Movebase<Tspace>
		{
			private:
				typedef Movebase<Tspace> base;
			protected:
				using base::spc;
				using base::pot;
				using base::w;
				using base::cnt;
				void _test( UnitTest & ) override;
				void _trialMove() override;
				void _acceptMove() override;
				void _rejectMove() override;
				Tmjson _json() override;
				double _energyChange() override;
				string _info() override;
				typedef std::map<string, Average<double> > map_type;
				map_type accmap;   //!< Group particle acceptance map
				map_type sqrmap_t; //!< Group mean square displacement map (translation)
				map_type sqrmap_r; //!< Group mean square displacement map (rotation)
				Group *igroup;
				double dp_rot;     //!< Rotational displament parameter
				double dp_trans;   //!< Translational displacement parameter
				double angle;      //!< Temporary storage for current angle
				Point dir;         //!< Translation directions (default: x=y=z=1). This will be set by setGroup()

			public:

				TranslateRotate( Energy::Energybase<Tspace> &, Tspace &, Tmjson & );
				void setGroup( Group & ); //!< Select Group to move
				bool groupWiseEnergy;  //!< Attempt to evaluate energy over groups from vector in Space (default=false)
				std::map<string, Point> directions; //!< Specify special group translation directions (default: x=y=z=1)
		};

		/**
		 * @brief Constructor
		 *
		 * The default JSON entry is read from section `moltransrot`
		 * with each element being the molecule name with the following
		 * values:
		 *
		 * Value      | Description
		 * :--------- | :-------------------------------------------------------------
		 * `dir`      | Move directions (default: "1 1 1" = xyz)
		 * `permol`   | Repeat move for each molecule in system (default: true)
		 * `prob`     | Probability of performing the move (default: 1)
		 * `dp`       | Translational displacement parameter (angstrom, default: 0)
		 * `dprot`    | Angular displacement parameter (radians, default: 0)
		 *
		 * Example:
		 *
		 *     moltransrot {
		 *       "water"   : { "dp":0.5, "dprot":0.5 },
		 *       "polymer" : { ... }
		 *     }
		 *
		 * Atomic displacement parameters are read from `AtomData`.
		 *
		 * @param e Energy function
		 * @param s Space
		 * @param j JSON object - typically `moves`.
		 */
		template<class Tspace>
			TranslateRotate<Tspace>::TranslateRotate(
					Energy::Energybase<Tspace> &e,
					Tspace &s, Tmjson &j ) : base(e, s)
		{

			base::title = "Group Rotation/Translation";
			base::w = 30;
			igroup = nullptr;
			groupWiseEnergy = false;

			base::fillMolList(j);// find molecules to be moved

			for ( auto &i : this->mollist )
			{ // loop over molecules to be moved
				string molname = spc->molList()[i.first].name;
				i.second.dp1 = j[molname]["dp"] | 0.0;
				i.second.dp2 = j[molname]["dprot"] | 0.0;
				if ( i.second.dp2 > 4 * pc::pi )    // no need to rotate more than
					i.second.dp2 = 4 * pc::pi;      // +/- 2 pi.
			}
		}

		template<class Tspace>
			void TranslateRotate<Tspace>::setGroup( Group &g )
			{
				assert(this->mollist.empty() && "Use either JSON data or setGroup");
				assert(!g.name.empty() && "Group should have a name.");
				assert(g.isMolecular());
				assert(spc->geo.sqdist(g.cm, g.cm_trial) < 1e-6 && "Trial CM mismatch");
				igroup = &g;
				if ( directions.find(g.name) != directions.end())
					dir = directions[g.name];
				else
					dir.x() = dir.y() = dir.z() = 1;
			}

		template<class Tspace>
			void TranslateRotate<Tspace>::_trialMove()
			{
				// if `mollist` has data, favor this over `setGroup()`
				// Note that `currentMolId` is set by Movebase::move()
				if ( !this->mollist.empty())
				{
					auto gvec = spc->findMolecules(this->currentMolId);
					assert(!gvec.empty());
					igroup = *slump.element(gvec.begin(), gvec.end());
					assert(!igroup->empty());
					auto it = this->mollist.find(this->currentMolId);
					if ( it != this->mollist.end())
					{
						dp_trans = it->second.dp1;
						dp_rot = it->second.dp2;
						dir = it->second.dir;
					}
				}

				assert(igroup != nullptr);
				Point p;

				vector<double> tempA, tempB;

				for (auto k : *igroup)
					for (auto l : *igroup)
						tempA.push_back( spc->geo.dist(spc->trial[k],spc->trial[l]) );

				if ( dp_rot > 1e-6 )
				{
					//cout << "CM before rotation:" << igroup->cm.transpose() << endl;
					p.ranunit(slump);             // random unit vector
					p = igroup->cm + p;                    // set endpoint for rotation
					angle = dp_rot * slump.half();
					igroup->rotate(*spc, p, angle);
					//cout << "CM after rotation:" << igroup->cm.transpose() << endl;
				}
				if ( dp_trans > 1e-6 )
				{
					p.x() = dir.x() * dp_trans * slump.half();
					p.y() = dir.y() * dp_trans * slump.half();
					p.z() = dir.z() * dp_trans * slump.half();
					igroup->translate(*spc, p);
				}


				for (auto k : *igroup)
					for (auto l : *igroup)
						tempB.push_back( spc->geo.dist(spc->trial[k],spc->trial[l]) );

				for(int k = 0; k < tempA.size(); k++)
					if(fabs(tempA.at(k) - tempB.at(k)) > 1e-7)
						cout << "Error in TranslateRotate!!" << endl;


				// register the moved group but set the number of moved particles
				// to zero. Doing so, it is assumed that all particles have been moved and
				// no internal energy calculation is performed.
				int g_index = spc->findIndex(igroup);
				base::change.mvGroup[g_index].clear();
			}

		template<class Tspace>
			void TranslateRotate<Tspace>::_acceptMove()
			{
				double r2 = spc->geo.sqdist(igroup->cm, igroup->cm_trial);
				sqrmap_t[igroup->name] += r2;
				sqrmap_r[igroup->name] += pow(angle * 180 / pc::pi, 2);
				accmap[igroup->name] += 1;
				igroup->accept(*spc);
			}

		template<class Tspace>
			void TranslateRotate<Tspace>::_rejectMove()
			{
				sqrmap_t[igroup->name] += 0;
				sqrmap_r[igroup->name] += 0;
				accmap[igroup->name] += 0;
				igroup->undo(*spc);
			}

		template<class Tspace>
			double TranslateRotate<Tspace>::_energyChange()
			{
				if ( dp_rot < 1e-6 && dp_trans < 1e-6 )
					return 0;

				return Energy::energyChange(*spc, *base::pot, base::change);

				// The code below is obsolete and will be removed in the future.
				/*#ifdef ENABLE_MPI
				  if (base::mpiPtr!=nullptr) {
				  double du=0;
				  auto s = Faunus::MPI::splitEven(*base::mpiPtr, spc->groupList().size());
				  for (auto i=s.first; i<=s.second; ++i) {
				  auto gi=spc->groupList()[i];
				  if (gi!=igroup)
				  du += pot->g2g(spc->trial, *gi, *igroup) - pot->g2g(spc->p, *gi, *igroup);
				  }
				  return (unew-uold) + Faunus::MPI::reduceDouble(*base::mpiPtr, du);
				  }
#endif*/
			}

		template<class Tspace>
			string TranslateRotate<Tspace>::_info()
			{
				using namespace textio;
				std::ostringstream o;
				o << pad(SUB, w, "Max. translation") << pm << dp_trans / 2 << textio::_angstrom << endl
					<< pad(SUB, w, "Max. rotation") << pm << dp_rot / 2 * 180 / pc::pi << textio::degrees << endl;
				if ( !directions.empty())
				{
					o << indent(SUB) << "Group Move directions:" << endl;
					for ( auto &m : directions )
						o << pad(SUBSUB, w - 2, m.first)
							<< m.second.transpose() << endl;
				}
				if ( cnt > 0 )
				{
					char l = 12;
					o << indent(SUB) << "Move Statistics:" << endl
						<< indent(SUBSUB) << std::left << setw(20) << "Group name" //<< string(20,' ')
						<< setw(l + 1) << "Acc. " + percent
						<< setw(l + 9) << rootof + bracket("dR" + squared) + "/" + angstrom
						<< setw(l + 5) << rootof + bracket("d" + theta + squared) + "/" + degrees << endl;
					for ( auto m : accmap )
					{
						string id = m.first;
						o << indent(SUBSUB) << std::left << setw(20) << id;
						o.precision(3);
						o << setw(l) << accmap[id].avg() * 100
							<< setw(l) << sqrt(sqrmap_t[id].avg())
							<< setw(l) << sqrt(sqrmap_r[id].avg()) << endl;
					}
				}
				return o.str();
			}

		template<class Tspace>
			Tmjson TranslateRotate<Tspace>::_json()
			{
				using namespace textio;
				Tmjson j;
				j = {
					{ this->title,
						{
							{ "max translation", pm + std::to_string(dp_trans/2) + _angstrom },
							{ "max rotation", pm + std::to_string(dp_rot/2*180/pc::pi) + degrees }
						}
					}
				};
				return j;
			}

		template<class Tspace>
			void TranslateRotate<Tspace>::_test( UnitTest &t )
			{
				string sec = textio::trim(base::title) + "_";
				for ( auto m : accmap )
				{
					string id = m.first,
					       idtrim = textio::trim(id) + "_";
					t(sec + idtrim + "acceptance", accmap[id].avg() * 100);
					t(sec + idtrim + "dRot", sqrt(sqrmap_r[id].avg()));
					t(sec + idtrim + "dTrans", sqrt(sqrmap_t[id].avg()));
				}
			}

		/**
		 * @brief Move that will swap conformation of a molecule
		 *
		 * This will swap between different molecular conformations
		 * as defined in `MoleculeData` with `traj` and `weight`.
		 * If defined, the weight
		 * distribution is respected, otherwise all conformations
		 * have equal intrinsic weight. Upon insertion, the new conformation
		 * is randomly oriented and placed on top of the mass-center of
		 * an exising molecule. That is, there is no mass center movement.
		 *
		 * The JSON input is identical to `Move::TranslateRotate` except that
		 * displacement parameters are ignored.
		 *
		 * @todo Add feature to align molecule on top of an exiting one
		 * @todo Expand `_info()` to show number of conformations
		 * @warning Weighted distributions untested and not verified for correctness
		 * @date Malmo, November 2016
		 */
		template<class Tspace, class base=TranslateRotate<Tspace> >
			class ConformationSwap : public base
		{

			private:

				using base::spc;
				using base::igroup; // group pointer to molecule being moved

				typedef MoleculeData<typename Tspace::ParticleVector> Tmoldata;
				RandomInserter<Tmoldata> inserter;

				void _trialMove() override
				{
					auto gvec = spc->findMolecules(base::currentMolId);
					assert(!gvec.empty());
					igroup = *slump.element(gvec.begin(), gvec.end());
					assert(igroup != nullptr); // make sure we really found a group

					if ( !igroup->empty())
					{
						assert( igroup->cm == igroup->cm_trial);
						inserter.offset = igroup->cm_trial;
						auto pnew = inserter(spc->geo, spc->p, spc->molecule[igroup->molId]); // get conformation

						if (pnew.size() == size_t(igroup->size()))
							std::copy( pnew.begin(), pnew.end(),
									spc->trial.begin() + igroup->front()); // override w. new conformation
						else
							throw std::runtime_error(base::title + ": conformation atom count mismatch");

						// this move shouldn't move mass centers, so let's check if this is true:
						igroup->cm_trial = Geometry::massCenter(spc->geo, spc->trial, *igroup);
						if ( (igroup->cm_trial - igroup->cm).norm()>1e-6 )
							throw std::runtime_error(base::title + ": unexpected mass center movement");
					}

					assert(spc->p.size() == spc->trial.size());
				}

				double _energyChange() override
				{
					double du = base::_energyChange();
					base::alternateReturnEnergy = du
						+ base::pot->g_internal(spc->trial, *igroup) - base::pot->g_internal(spc->p, *igroup);
					return du;
				}

			public:

				ConformationSwap( Energy::Energybase<Tspace> &e, Tspace &s, Tmjson &j ) : base(e, s, j)
			{
				base::title = "Conformation Swap";
				inserter.checkOverlap = false; // will be done by _energyChange()
				inserter.dir = {0, 0, 0};      // initial placement at origo
				inserter.rotate = true;        // rotate conformation randomly
				base::useAlternativeReturnEnergy=true; // yes, we Metropolis doesn't need internal energy change
				base::dp_trans = 1; // if zero, base::energyChange() returns 0 which we don't want!
			}
		};

		/**
		 * @brief Translates/rotates many groups simultaneously
		 *
		 * By default, this move till attempt to translate/rotate all
		 * molecular groups simultaneousny. It can however be restricted
		 * to only a subset via the `setGroup` function.
		 */
		template<class Tspace>
			class TranslateRotateNbody : public TranslateRotate<Tspace>
		{
			protected:
				typedef TranslateRotate<Tspace> base;
				typedef opair<Group *> Tpair;
				using base::pot;
				using base::spc;

				typename base::map_type angle2; //!< Temporary storage for angular movement
				vector<Group *> gVec;   //!< Vector of groups to move

				void _trialMove() override
				{
					angle2.clear();
					for ( auto g : gVec )
					{
						if ( g->isMolecular())
						{
							Point p;
							if ( base::dp_rot > 1e-6 )
							{
								p.ranunit(slump);        // random unit vector
								p = g->cm + p;                    // set endpoint for rotation
								double angle = base::dp_rot * slump.half();
								g->rotate(*base::spc, p, angle);
								angle2[g->name] += pow(angle * 180 / pc::pi, 2); // sum angular movement^2
							}
							if ( base::dp_trans > 1e-6 )
							{
								p.ranunit(slump);
								p = base::dp_trans * p.cwiseProduct(base::dir);
								g->translate(*base::spc, p);
							}
						}
					}
				}

				void _acceptMove() override
				{
					std::map<string, double> r2;
					for ( auto g : gVec )
					{
						r2[g->name] += base::spc->geo.sqdist(g->cm, g->cm_trial);
						g->accept(*base::spc);
						base::accmap[g->name] += 1;
					}
					for ( auto &i : r2 )
						base::sqrmap_t[i.first] += i.second;
					for ( auto &i : angle2 )
						base::sqrmap_r[i.first] += i.second;
				}

				void _rejectMove() override
				{
					std::set<string> names; // unique group names
					for ( auto g : gVec )
					{
						names.insert(g->name);
						g->undo(*base::spc);
						base::accmap[g->name] += 0;
					}
					for ( auto n : names )
					{
						base::sqrmap_t[n] += 0;
						base::sqrmap_r[n] += 0;
					}
				}

				string _info() override
				{
					std::ostringstream o;
					o << textio::pad(textio::SUB, base::w, "Number of groups") << gVec.size() << endl
						<< base::_info();
					return o.str();
				}

				double _energyChange() override
				{
					double du = 0;

					// check for container collision
					for ( auto gi : gVec )
						for ( auto i : *gi )
							if ( spc->geo.collision(spc->trial[i], spc->trial[i].radius, Geometry::Geometrybase::BOUNDARY))
								return pc::infty;

					// all groups not in gVec
					auto othergroups = erase_range(spc->groupList(), gVec);

					for ( auto gi : gVec )
					{
						// external energy on moved groups
						du += pot->g_external(spc->trial, *gi) - pot->g_external(spc->p, *gi);
						// moved<->all static groups
						for ( auto gj : othergroups )
							du += pot->g2g(spc->trial, *gi, *gj) - pot->g2g(spc->p, *gi, *gj);
					}
					// moved<->moved
					for ( auto i = gVec.begin(); i != gVec.end(); ++i )
						for ( auto j = i; ++j != gVec.end(); )
							du += pot->g2g(spc->trial, **i, **j) - pot->g2g(spc->p, **i, **j);
					return du;
				}

				void setGroup( std::vector<Group *> &v )
				{
					gVec.clear();
					for ( auto i : v )
						if ( i->isMolecular())
							gVec.push_back(i);
				}

			public:
				TranslateRotateNbody( Energy::Energybase<Tspace> &e, Tspace &s, Tmjson &j ) : base(e, s, j)
			{
				base::title += " (N-body)";
				setGroup(s.groupList());
			}
		};

		/**
		 * @brief Symmetric twobody move
		 *
		 * This will move exactly two groups at the time by symmetrically displacing
		 * them along the vector connecting their mass-centers. The move will, if a
		 * rotational displacement paramter is set, also rotate the two groups.
		 * The JSON input is identical to the `Move::TranslateRotate` move but
		 * exactly two molecules must be given.
		 */
		template<class Tspace, class base=TranslateRotateNbody<Tspace> >
			class TranslateRotateTwobody : public base
		{
			protected:
				using base::gVec;
				using base::spc;
				using base::angle2;
				using base::dp_trans;
				using base::dp_rot;

				void _trialMove() override
				{
					assert(gVec.size() == 2);

					// displacement vector between mass centers
					Point R = spc->geo.vdist(gVec[0]->cm, gVec[1]->cm);
					R.normalize();
					R = R * dp_trans * slump.half();

					angle2.clear();
					for ( size_t i = 0; i < 2; i++ )
					{
						if ( gVec[i]->isMolecular())
						{
							auto it = this->mollist.find(gVec[i]->molId);
							dp_rot = it->second.dp2;

							if ( dp_rot > 1e-6 )
							{
								Point p;
								p.ranunit(slump);        // random unit vector
								p = gVec[i]->cm + p;                    // set endpoint for rotation
								double angle = dp_rot * slump.half();
								gVec[i]->rotate(*spc, p, angle);
								angle2[gVec[i]->name] += pow(angle * 180 / pc::pi, 2); // sum angular movement^2
							}
							if ( dp_trans > 1e-6 )
							{
								if ( i == 1 )
									R = -R;
								gVec[i]->translate(*spc, R);
							}
						}
					}
				}

			public:
				TranslateRotateTwobody( Energy::Energybase<Tspace> &e, Tspace &s, Tmjson &j ) : base(e, s, j)
			{
				this->title += " (2-body, symmetric)";
				assert(this->mollist.size() == 2 && "Specify exactly two molecules");

				decltype(gVec) g;
				dp_trans = 1e20;
				for ( auto &m : this->mollist )
				{
					auto gi = spc->findFirstMolecule(m.first);
					assert(gi != nullptr);
					g.push_back(gi);
					// set translational displacement to smallest value
					if ( m.second.dp1 < dp_trans )
						dp_trans = m.second.dp1;
				}
				base::setGroup(g);

				assert(gVec.size() == 2);
				assert(gVec[0]->molId != gVec[1]->molId && "Molecules must have different id's");
			}
		};

		/**
		 * @brief Combined rotation and rotation of groups and mobile species around it
		 *
		 * This class will do a combined translational and rotational move of a group
		 * along with atomic particles surrounding it.
		 * To specify where to look for clustered particles, use the `setMobile()`
		 * function. Whether particles are considered part of the cluster is
		 * determined by the private virtual function `ClusterProbability()`.
		 * By default this is a simple step function with P=1 when an atomic particle
		 * is within a certain threshold to a particle in the main group; P=0 otherwise.
		 *
		 * The implemented cluster algorithm is general - see Frenkel&Smith,
		 * 2nd ed, p405 - and derived classes can re-implement `ClusterProbability()`
		 * for arbitrary probability functions.
		 *
		 * In additon to the molecular keywords in
		 * `Moves::TranslateRotate`, the JSON input is searched
		 * the following in `moves/moltransrotcluster`,
		 *
		 * Keyword         | Description
		 * :---------------| :----------------
		 * `clusterradius` | Surface threshold from mobile ion to particle in group (angstrom)
		 * `clustergroup`  | Group containing atomic particles to be moved with the main molecule
		 *
		 * @todo Energy evaluation puts all moved particles in an index vector used
		 * to sum the interaction energy with static particles. This could be optimized
		 * by only adding mobile ions and calculate i.e. group-group interactions in a
		 * more traditional (and faster) way.
		 */
		template<class Tspace>
			class TranslateRotateCluster : public TranslateRotate<Tspace>
		{
			protected:
				typedef TranslateRotate<Tspace> base;
				typedef typename Tspace::ParticleVector Tpvec;
				using base::pot;
				using base::w;
				using base::cnt;
				using base::igroup;
				using base::dp_trans;
				using base::dp_rot;
				using base::dir;
				Geometry::QuaternionRotate vrot;
				vector<int> cindex; //!< index of mobile ions to move with group
				void _trialMove() override;
				void _acceptMove() override;
				void _rejectMove() override;
				double _energyChange() override;
				string _info() override;
				Average<double> avgsize; //!< Average number of ions in cluster
				Average<double> avgbias; //!< Average bias
				Group *gmobile;          //!< Pointer to group with potential cluster particles
				virtual double ClusterProbability( Tpvec &, int ); //!< Probability that particle index belongs to cluster
			public:
				using base::spc;
				TranslateRotateCluster( Energy::Energybase<Tspace> &, Tspace &, Tmjson &j );
				virtual ~TranslateRotateCluster();
				void setMobile( Group & );  //!< Pool of atomic species to move with the main group
				double threshold;        //!< Distance between particles to define a cluster
		};

		template<class Tspace>
			TranslateRotateCluster<Tspace>::TranslateRotateCluster(
					Energy::Energybase<Tspace> &e, Tspace &s, Tmjson &j ) : base(e, s, j)
			{
				base::title = "Cluster " + base::title;
				base::cite = "doi:10/cj9gnn";
				gmobile = nullptr;

				auto m = j;
				base::fillMolList(m);// find molecules to be moved

				if ( this->mollist.size() != 1 )
					throw std::runtime_error(base::title+": only one cluster group allowed");
				else
				{
					for ( auto &i : this->mollist )
					{
						string molname = spc->molList()[i.first].name;
						string mobname = m[molname].at("clustergroup");
						threshold = m[molname].at("threshold");
						dp_trans = m[molname].at("dp");
						dp_rot = m[molname].at("dprot");
						dir << j.value("dir", string("1 1 1") );  // magic!

						auto mob = spc->findMolecules(mobname); // mobile atoms to include in move
						if ( mob.size() == 1 )
							setMobile(*mob.front());
						else
							throw std::runtime_error(base::title+": atomic group ill defined");
					}
				}
			}

		template<class Tspace>
			TranslateRotateCluster<Tspace>::~TranslateRotateCluster() {}

		template<class Tspace>
			void TranslateRotateCluster<Tspace>::setMobile( Group &g )
			{
				gmobile = &g;
			}

		template<class Tspace>
			string TranslateRotateCluster<Tspace>::_info()
			{
				using namespace textio;
				std::ostringstream o;
				o << base::_info() << endl;
				o << pad(SUB, w, "Cluster threshold") << threshold << _angstrom << endl;
				if ( cnt > 0 )
				{
					o << pad(SUB, w, "Average cluster size") << avgsize.avg() << endl;
					if ( threshold > 1e-9 )
						o << pad(SUB, w, "Average bias") << avgbias.avg() << " (0=reject, 1=accept)\n";
				}
				return o.str();
			}

		template<class Tspace>
			void TranslateRotateCluster<Tspace>::_trialMove()
			{

				// if `mollist` has data, favor this over `setGroup()`
				// Note that `currentMolId` is set by Movebase::move()
				if ( !this->mollist.empty())
				{
					auto gvec = spc->findMolecules(this->currentMolId);
					assert(!gvec.empty());
					igroup = *slump.element(gvec.begin(), gvec.end());
					assert(!igroup->empty());
					//auto it = this->mollist.find(this->currentMolId);
				}

				assert(gmobile != nullptr && "Cluster group not defined");
				assert(igroup != nullptr && "Group to move not defined");

				// find clustered particles
				cindex.clear();
				for ( auto i : *gmobile )
					if ( ClusterProbability(spc->p, i) > slump())
						cindex.push_back(i); // generate cluster list 
				// rotation
				Point p;
				if ( dp_rot > 1e-6 )
				{


					base::angle = dp_rot * slump.half();
					p.ranunit(slump);
					p = igroup->cm + p; // set endpoint for rotation
					igroup->rotate(*spc, p, base::angle);
					vrot.setAxis(spc->geo, igroup->cm, p, base::angle); // rot. around line CM->p
					for ( auto i : cindex )
						spc->trial[i] = vrot(spc->trial[i]); // rotate
				}

				// translation
				if ( dp_trans > 1e-6 )
				{
					p.x() = dir.x() * dp_trans * slump.half();
					p.y() = dir.y() * dp_trans * slump.half();
					p.z() = dir.z() * dp_trans * slump.half();
					igroup->translate(*spc, p);
					for ( auto i : cindex )
						spc->trial[i].translate(spc->geo, p);
				}
			}

		template<class Tspace>
			void TranslateRotateCluster<Tspace>::_acceptMove()
			{
				base::_acceptMove();
				for ( auto i : cindex )
					spc->p[i] = spc->trial[i];
				avgsize += cindex.size();
			}

		template<class Tspace>
			void TranslateRotateCluster<Tspace>::_rejectMove()
			{
				base::_rejectMove();
				for ( auto i : cindex )
					spc->trial[i] = spc->p[i];
			}

		template<class Tspace>
			double TranslateRotateCluster<Tspace>::_energyChange()
			{
				double bias = 1;             // cluster bias -- see Frenkel 2nd ed, p.405
				vector<int> imoved = cindex; // index of moved particles
				for ( auto l : *gmobile )    // mobile index, "l", NOT in cluster (Frenkel's "k" is the main group)
					if ( std::find(cindex.begin(), cindex.end(), l) == cindex.end())
						bias *= (1 - ClusterProbability(spc->trial, l)) / (1 - ClusterProbability(spc->p, l));
				avgbias += bias;
				if ( bias < 1e-7 )
					return pc::infty;        // don't bother to continue with energy calculation

				if ( dp_rot < 1e-6 && dp_trans < 1e-6 )
					return 0;

				for ( auto i : *igroup )     // Add macromolecule to list of moved particle index
					imoved.push_back(i);

				// container boundary collision?
				for ( auto i : imoved )
					if ( spc->geo.collision(spc->trial[i], spc->trial[i].radius, Geometry::Geometrybase::BOUNDARY))
						return pc::infty;

				// external potential on macromolecule
				double unew = pot->g_external(spc->trial, *igroup);
				if ( unew == pc::infty )
					return pc::infty; //early rejection!
				double uold = pot->g_external(spc->p, *igroup);

				// external potentia on clustered atomic species
				for ( auto i : cindex )
				{
					uold += pot->i_external(spc->p, i);
					unew += pot->i_external(spc->trial, i);
				}

				// pair energy between static and moved particles
				// note: this could be optimized!
				double du = 0;
#pragma omp parallel for reduction (+:du)
				for ( int j = 0; j < (int) spc->p.size(); j++ )
					if ( std::find(imoved.begin(), imoved.end(), j) == imoved.end())
						for ( auto i : imoved )
							du += pot->i2i(spc->trial, i, j) - pot->i2i(spc->p, i, j);
				return unew - uold + du - log(bias); // exp[ -( dU-log(bias) ) ] = exp(-dU)*bias
			}

		/**
		 * This is the default function for determining the probability, P,
		 * that a mobile particle is considered part of the cluster. This
		 * is here a simple distance critera but derived classes can reimplement
		 * this (virtual) function to arbitrary probability functions.
		 */
		template<class Tspace>
			double TranslateRotateCluster<Tspace>::ClusterProbability( Tpvec &p, int i )
			{
				for ( auto j : *igroup ) // loop over main group
					if ( i != j )
					{
						double r = threshold + p[i].radius + p[j].radius;
						if ( spc->geo.sqdist(p[i], p[j]) < r * r )
							return 1;
					}
				return 0;
			}

		template<class Tspace>
			class ClusterMove : public TranslateRotate<Tspace>
		{
			protected:
				typedef TranslateRotate<Tspace> base;
				typedef typename Tspace::ParticleVector Tpvec;
				typedef PropertyBase::Tid Tid;
				using base::pot;
				using base::w;
				using base::cnt;
				using base::igroup;
				vector<double> dp_trans, dp_rot; // displacements for the different type of groups
				vector<Point> dir;
				vector<Group *> cindex; //!< index of mobile molecules to move with group
				void _trialMove() override;
				void _acceptMove() override;
				void _rejectMove() override;
				double _energyChange() override;
				void getClusterAroundMolecule(Group *);
				string _info() override;
				Average<double> avgsize; //!< Average number of ions in cluster
				Average<double> avgbias; //!< Average bias
				vector<vector<Tid>> gstatic;          //!< Pointer to group with potential cluster particles
				virtual double ClusterProbability(Group &, Tpvec &, int ); //!< Probability that particle index belongs to cluster
			public:
				using base::spc;
				ClusterMove( Energy::Energybase<Tspace> &, Tspace &, Tmjson &j );
				virtual ~ClusterMove();
				vector<double> threshold;        //!< Distance between particles to define a cluster
		};

		template<class Tspace>
			ClusterMove<Tspace>::ClusterMove(
					Energy::Energybase<Tspace> &e, Tspace &s, Tmjson &j ) : base(e, s, j)
			{
				base::title = "Cluster " + base::title;
				base::cite = "doi:10/cj9gnn";
				gstatic.resize(spc->molecule.size());
				threshold.resize(spc->molecule.size());
				dp_rot.resize(spc->molecule.size());
				dp_trans.resize(spc->molecule.size());
				dir.resize(spc->molecule.size());
				base::useAlternativeReturnEnergy=true; // yes, we Metropolis don't need internal energy change
				for(unsigned int i = 0; i < gstatic.size(); i++)
					gstatic.at(i).resize(0);

				auto m = j;
				base::fillMolList(m);// find molecules to be moved

				for ( auto &i : this->mollist )
				{
					string centername = spc->molList()[i.first].name;
					auto staticmols = m[centername]["staticmol"];

					if (m.count("staticmol")>0)
						if (m["staticmol"].is_array()) {
							for (auto &name : m["staticmol"]) {
								string a = name.get<string>() ;
								gstatic.at(int(i.first)).push_back(spc->findMolecules(a).at(0)->molId);	
							}
						}


					threshold.at(int(i.first)) = m[centername].at("threshold");
					dp_trans.at(int(i.first)) = m[centername].at("dp");
					dp_rot.at(int(i.first)) = m[centername].at("dprot");
					Point temp(0,0,0);
					temp  << j.value("dir", string("1 1 1"));
					dir.at(int(i.first)) = temp;  // magic!
				}

			}

		template<class Tspace>
			ClusterMove<Tspace>::~ClusterMove() {}


		template<class Tspace>
			string ClusterMove<Tspace>::_info()
			{
				using namespace textio;
				std::ostringstream o;
				o << base::_info() << endl;
				for (int i = 0; i < (int)threshold.size(); i++)
					o << pad(SUB, w, "Cluster threshold, mol ") << threshold.at(i) << _angstrom << endl;
				if ( cnt > 0 )
				{
					o << pad(SUB, w, "Average cluster size") << avgsize.avg() << endl;
					if ( threshold.at(0) > 1e-9 )
						o << pad(SUB, w, "Average bias") << avgbias.avg() << " (0=reject, 1=accept)\n";
				}
				return o.str();
			}

		template<class Tspace>
			void ClusterMove<Tspace>::getClusterAroundMolecule(Group *g)
			{
				int cntI = 0;
				for ( auto &i : spc->molList() ) { 	// For every type of molecule ...
					bool is_static = false; 	// Start to assume the molecule is not prohibited from being in cluster around molecule 'g'
					for( auto k : gstatic.at(int(g->molId)) ) { // For every group which is prohibited from being in cluster around molecule 'g' ...
						if(int(k) == cntI) { 	// Check if molecule 'i' is indeed prohibited from being in the cluster around molecule 'g'
							is_static = true; // If so then change assumed status
							break;		// No need to go through more elements in the list of prohibited molecules
						}
					}
					if(is_static)
						continue;
					// molecule type 'i' is not prohibited from being in the cluster around molecule 'g'

					auto gvec = spc->findMolecules(cntI); 	// Find all molecules of type 'i'
					for( auto g0 : gvec) { 			// For every molecule of type 'i' ...
						for(auto index : *g0) { 	// For every atom in molecule ...
							if ( ClusterProbability(*g,spc->p, index) > slump()) {
								// Include atom 'index' and thus also molecule 'g0'
								bool in_cluster = false;			
								for( unsigned int m = 0; m < cindex.size(); m++ ) {
									if( *cindex.at(m) == *g0 ) {
										in_cluster = true;
										break;	
									}	
								} 
								if(in_cluster)
									break;
								cindex.push_back(g0); 			// If not then add molecule to cluster-list
								getClusterAroundMolecule(g0);	 	// Get cluster around added molecule
								break; // No need to go through any more atoms in the molecule
							}
						}
					}
					cntI++;
				}
			}


		template<class Tspace>
			void ClusterMove<Tspace>::_trialMove()
			{
				// if `mollist` has data, favor this over `setGroup()`
				// Note that `currentMolId` is set by Movebase::move()
				if ( !this->mollist.empty())
				{
					auto gvec = spc->findMolecules(this->currentMolId);
					assert(!gvec.empty());
					igroup = *slump.element(gvec.begin(), gvec.end());
					assert(!igroup->empty());
				}

				assert(igroup != nullptr && "Group to move not defined");
				// find clustered particlesi
				cindex.clear();
				cindex.push_back(igroup);
				getClusterAroundMolecule(igroup);

				// Check - can be removed
				for(auto i : cindex) {
					Point cm_temp = i->cm - i->cm_trial;
					if( cm_temp.norm() > 1e-7 )
						cout << "Molecule and trial-molecule are not located at the same place!" << endl;
					for(auto k : *i) {
						Point temp = spc->p[k] - spc->trial[k];
						if( temp.norm() > 1e-7 )
							cout << "Particle and trial-particle are not located at the same place!" << endl;
					}
				}

				// rotation
				Point p;

				// Looking for the longest distance between particles in the cluster-groups 
				// (to see if they are larger than half the box-length)

				if ( dp_rot.at(int(this->currentMolId)) > 1e-6 )
				{
					base::angle = dp_rot.at(int(this->currentMolId)) * slump.half();

					// Get maximum distance within any molecule
					double ald = 0.0;
					for (auto k : cindex)
						for (auto l : *k )
							for (auto m : *k ) {
								double ald_t = spc->geo.dist(spc->trial[l],spc->trial[m]);
								if(ald_t > ald)
									ald = ald_t;
							}

					// Get maximum distance between cluster atoms and cluster center
					double ld = 0.0;
					Point cm = Geometry::trigoComCluster(spc->geo,spc->p, cindex);
					for ( auto i : cindex ) {
						for (auto l : *i) {
							double dist_t = spc->geo.dist(cm,spc->p[l]);
							if(dist_t > ld)
								ld = dist_t;
						}
					}
					ld  += ald; // add maximum internal distance to avoid that atoms in molecule are split up by PBC during rotation
					
					bool sqrt4_big = false;  // Is the cluster bigger than half the smallest box length?  
					if( (ld > spc->geo.len.x()*0.5) || ( ld > spc->geo.len.y()*0.5 ) || ( ld > spc->geo.len.z()*0.5 ))
						sqrt4_big = true;       // if some inter-atomic distance exceeds half the box length in any direction, cluster is considered "too big"

					if (!sqrt4_big) {          // we rotate only if the cluster is not too big
						Point cm = Geometry::trigoComCluster(spc->geo,spc->p, cindex);
						for ( auto i : cindex ) {

							Point cmb = i->cm_trial; // only for check
							Point ntrb = spc->trial[i->back()]; // only for check

							vector<double> tempA, tempB; // only for check

							// only for check
							for (auto k : *i)
								for (auto l : *i)
									tempA.push_back( spc->geo.dist(spc->trial[k],spc->trial[l]) );

							p.ranunit(slump);
							i->rotatecluster(*spc, cm+p, base::angle, cm);


							Point cm2 = Geometry::trigoComCluster(spc->geo,spc->trial, cindex);
							// only for check
							for (auto k : *i)
								for (auto l : *i)
									tempB.push_back( spc->geo.dist(spc->trial[k],spc->trial[l]) );

							int cnt = 0;
							for (auto k : *i) {
								for (auto l : *i) {
									if(fabs(tempA.at(cnt) - tempB.at(cnt)) > 1e-7) {
										cout << "Error in ClusterMove! " << k << ", " << l << endl;
										cout << "coordB: " << spc->p[k].transpose() << ", " << spc->p[l].transpose() << endl;
										cout << "coordA: " << spc->trial[k].transpose() << ", " << spc->trial[l].transpose() << endl;
										cout << "cmrotB:" << cm.transpose() << endl;
										cout << "cmrotA:" << cm2.transpose() << endl;
										cout << tempA.at(cnt) << "/" << tempB.at(cnt) << endl;
									}
									cnt++;                                                                         
								}
							}
							Point cma = i->cm_trial;
							Point ntra = spc->trial[i->back()];
							if (fabs((cma-ntra).norm()) > 1e-8 || fabs((cmb-ntrb).norm()) > 1e-8) {
								cout <<  "cma:" << cma.transpose() << endl;
								cout <<  "cmb:" << cmb.transpose() << endl;
								cout <<  "ntra:" << ntra.transpose() << endl;
								cout <<  "ntrb:" << ntrb.transpose() << endl;
							}

						}
					} 
				}
				// translation
				else {
					Point u;
					u.ranunit(slump);
					p = dp_trans.at(int(this->currentMolId)) * u * 0.5;
					for ( auto i : cindex )
						i->translate(*spc,p); 
				}
			}

		template<class Tspace>
			void ClusterMove<Tspace>::_acceptMove()
			{
				base::_acceptMove();
				for ( auto k : cindex ) {
					k->accept(*spc);
				}
				avgsize += cindex.size();
			}

		template<class Tspace>
			void ClusterMove<Tspace>::_rejectMove()
			{
				base::_rejectMove();
				for ( auto k : cindex ) {
					k->undo(*spc);
				}
			}

		template<class Tspace>
			double ClusterMove<Tspace>::_energyChange()
			{
				double bias = 1;             // cluster bias -- see Frenkel 2nd ed, p.405

				for (auto k : cindex ) {
					for(auto l : spc->groupList() ) {
						bool in_cluster = false;
						for(auto kt : cindex)
							if(*kt == *l) {
								in_cluster = true;
								break;
							} 

						if(in_cluster)
							break;
						// only gets here if 'l' is not in the cluster

						bool is_static = false;  // We check if 'l' is in the static list for this particular group
						for( auto m : gstatic.at(int(k->molId)))  {
							if( m == l->molId ) {
								is_static = true;
								break;
							}
						}
						if( !is_static ) {
							// 'l' is mobile and not in the cluster
							double a = 1.0;
							double b = 1.0;
							for(auto t : *l){ // for every atom in the 'l'-molecule...
								double at = ClusterProbability(*k,spc->trial, t); // Probability that atom 't' is in the trial-configuration cluster
								double bt = ClusterProbability(*k,spc->p, t);     // Probability that atom 't' is in the old-configuration cluster
								a *= (1.0 - at); // Multiply with probability that atom 't' is not in the trial-configuration cluster
								b *= (1.0 - bt); // Multiply with probability that atom 't' is not in the old-configuration cluster
							}
							a = 1.0 - a; // Probability that we included any of the trial-atoms in molecule 'l'
							b = 1.0 - b; // Probability that we included any of the old-atoms in molecule 'l'

							// Special cases for 'a' and/or 'b' is one/zero
							if( ( fabs(a-1.0) < 1e-9 ) && ( fabs(b-1.0) < 1e-9 ) ) {
								bias *= 1.0;
								continue;
							}
							if( ( fabs(a-1.0) < 1e-9 ) && ( fabs(b) < 1e-9 ) ) {
								return pc::infty;
							}
							if( ( fabs(a) < 1e-9 ) && ( fabs(b-1.0) < 1e-9 ) ) {
								return pc::infty;
							}
							if( ( fabs(a) < 1e-9 ) && ( fabs(b) < 1e-9 ) ) {
								bias *= 1.0;
								continue;
							}

							// Multiply bias (see Frenkel)
							bias *= (1-a)/(1-b);
						}
					}
				}

				avgbias += bias;
				if ( bias < 1e-7 )
					return pc::infty;        // don't bother to continue with energy calculation

				if ( dp_rot.at(int(this->currentMolId)) < 1e-6 && dp_trans.at(int(this->currentMolId)) < 1e-6 )
					return 0;

				// container boundary collision?
				for ( auto k : cindex ) 
					for( auto i : *k )   
						if ( spc->geo.collision(spc->trial[i], spc->trial[i].radius, Geometry::Geometrybase::BOUNDARY))
							return pc::infty;


				// external potential on macromoleculei
				double uext_new = 0.0;
				for ( auto k : cindex ) 
					uext_new += pot->g_external(spc->trial, *k);

				if ( uext_new == pc::infty )
					return pc::infty; //early rejection!
				double uext_old = 0.0;
				for ( auto k : cindex ) 
					uext_old += pot->g_external(spc->p, *k);

				// pair energy between static and moved particles
				// note: this could be optimized!
				double u_c2nc_new = 0;   //cluster to non-cluster molecules
				double u_c2nc_old = 0;
#pragma omp parallel for reduction (+:du)

				for (auto i : cindex)  {
					for (auto j : spc->groupList()) {
						bool in_cluster = false;
						for(auto t : cindex) {
							if(*j == *t) {
								in_cluster = true;
								break;
							}
						}
						if(!in_cluster) {
							u_c2nc_new += pot->g2g(spc->trial,*i,*j);
							u_c2nc_old += pot->g2g(spc->p,*i,*j) ;
						}
					}
				}

				double u_int_cluster_new = 0.0;   //cluster to cluster (internal)
				double u_int_cluster_old = 0.0;
				for(auto i : cindex)
					for(auto j : cindex)
						if( !(*i == *j) ) {
							u_int_cluster_new += 0.5* pot->g2g(spc->trial,*i,*j);
							u_int_cluster_old += 0.5*pot->g2g(spc->p,*i,*j) ;
						}

				uext_old += u_int_cluster_old;
				uext_new += u_int_cluster_new;

				double du = u_c2nc_new - u_c2nc_old;

				base::alternateReturnEnergy = ( uext_new - uext_old + du );
				return ( uext_new - uext_old + du - log(bias) ); // exp[ -( dU-log(bias) ) ] = exp(-dU)*bias

			}

		/**
		 * This is the default function for determining the probability, P,
		 * that a mobile particle is considered part of the cluster. This
		 * is here a simple distance critera but derived classes can reimplement
		 * this (virtual) function to arbitrary probability functions.
		 */
		template<class Tspace>
			double ClusterMove<Tspace>::ClusterProbability(Group &centergroup, Tpvec &p, int i )
			{
				for ( auto j : centergroup ) // loop over main group
					if ( i != j )
					{
						double r = threshold.at(int(centergroup.molId)) + p[i].radius + p[j].radius;
						if ( spc->geo.sqdist(p[i], p[j]) < r * r )
							return 1.0;
					}
				return 0.0;
			}




		/**
		 * @brief Rotate/translate group along with an extra group
		 *
		 * This will rotate/translate a group A around its mass center and, if
		 * defined, also an extra group, B. This can be useful for sampling groups
		 * joined together with springs, for example a polymer (B) joined to a
		 * protein (A). The group B can consist of many molecules/chains as
		 * long as these are continuous in the particle vector.
		 *
		 * @date Malmo 2014
		 */
		template<class Tspace>
			class TranslateRotateGroupCluster : public TranslateRotateCluster<Tspace>
		{
			private:
				typedef TranslateRotateCluster<Tspace> base;

				void _acceptMove()
				{
					base::_acceptMove();
					for ( auto i : base::spc->groupList())
						i->setMassCenter(*base::spc);
				}

				string _info() override { return base::base::_info(); }

				double ClusterProbability( typename base::Tpvec &p, int i ) override { return 1; }

			public:
				TranslateRotateGroupCluster( Tmjson &j, Energy::Energybase<Tspace> &e,
						Tspace &s ) : base(j, e, s)
			{
				base::title = "Translate-Rotate w. extra group";
			}
		};

		/**
		 * @brief Non-rejective cluster translation.
		 *
		 * This type of move will attempt to translate collective sets of macromolecules that
		 * with a symmetric transition matrix (no flow through the clusters).
		 * See detailed description [here](http://dx.doi.org/10/fthw8k).
		 *
		 * Setting the boolean `skipEnergyUpdate` to true (default is false) updates of the
		 * total energy are skipped to speed up the move.
		 * While this has no influence on the Markov chain it will cause an apparent energy
		 * drift. It is recommended that this is enabled only for long production runs after
		 * having properly checked that no drifts occur with `skipEnergyUpdate=false`.
		 *
		 * Upon construction the following keywords are read json section `moves/ctransnr`:
		 *
		 * Keyword     | Description
		 * :-----------| :---------------------------------------------
		 * `dp`        | Displacement parameter (default: 0)
		 * `skipenergy`| Skip energy update, see above (default: false)
		 * `prob`      | Runfraction (default: 1.0)
		 *
		 * @note Requirements for usage:
		 * - Compatible only with purely molecular systems
		 * - Works only with periodic containers
		 * - External potentials are ignored
		 *
		 * @author Bjoern Persson
		 * @date Lund 2009-2010
		 * @todo Energy calc. before and after can be optimized by only looping over `moved` with
		 * `remaining`
		 */
		template<class Tspace>
			class ClusterTranslateNR : public Movebase<Tspace>
		{
			private:
				typedef Movebase<Tspace> base;
				using base::w;
				using base::spc;
				using base::pot;
				vector<int> moved, remaining;
				void _trialMove() override;

				void _acceptMove() override {}

				void _rejectMove() override {}

				double _energyChange() override { return 0; }

				string _info() override;
				Average<double> movefrac; //!< Fraction of particles moved
				double dp;                //!< Displacement parameter [aa]
				vector<Group *>
					g;         //!< Group of molecules to move. Currently this needs to be ALL groups in the system!!
			public:
				ClusterTranslateNR( Energy::Energybase<Tspace> &, Tspace &, Tmjson & );
				bool skipEnergyUpdate;    //!< True if energy updates should be skipped (faster evaluation!)
		};

		/** @brief Constructor */
		template<class Tspace>
			ClusterTranslateNR<Tspace>::ClusterTranslateNR( Energy::Energybase<Tspace> &e, Tspace &s, Tmjson &j ) : base(e, s)
		{
			auto _j = j;
			base::title = "Rejection Free Cluster Translation";
			base::cite = "doi:10/fthw8k";
			base::useAlternativeReturnEnergy = true;
			base::runfraction = _j["prob"] | 1.0;
			skipEnergyUpdate = _j["skipenergy"] | false;
			dp = _j.at("dp");
			if ( dp < 1e-6 )
				base::runfraction = 0;
			g = spc->groupList(); // currently ALL groups in the system will be moved!
		}

		template<class Tspace>
			string ClusterTranslateNR<Tspace>::_info()
			{
				using namespace textio;
				std::ostringstream o;
				o << pad(SUB, w, "Displacement") << dp << _angstrom << endl
					<< pad(SUB, w, "Skip energy update") << std::boolalpha
					<< skipEnergyUpdate << endl;
				if ( movefrac.cnt > 0 )
				{
					o << pad(SUB, w, "Move fraction") << movefrac.avg() * 100 << percent << endl
						<< pad(SUB, w, "Avg. moved groups") << movefrac.avg() * spc->groupList().size() << endl;
				}
				return o.str();
			}

		template<class Tspace>
			void ClusterTranslateNR<Tspace>::_trialMove()
			{
				double du = 0;
				g = spc->groupList();
				moved.clear();
				remaining.resize(g.size());

				for ( size_t i = 0; i < g.size(); i++ )
				{
					remaining[i] = i;
					if ( base::cnt <= 1 )
						g[i]->setMassCenter(*spc);
				}

				if ( skipEnergyUpdate == false )
#pragma omp parallel for reduction (+:du) schedule (dynamic)
					for ( size_t i = 0; i < g.size() - 1; i++ )
						for ( size_t j = i + 1; j < g.size(); j++ )
							du -= pot->g2g(spc->p, *g[i], *g[j]);

				Point ip(dp, dp, dp);
				ip.x() *= slump.half();
				ip.y() *= slump.half();
				ip.z() *= slump.half();

				int f = slump() * remaining.size();
				moved.push_back(remaining[f]);
				remaining.erase(remaining.begin() + f);    // Pick first index in m to move

				for ( size_t i = 0; i < moved.size(); i++ )
				{
					g[moved[i]]->translate(*spc, ip);
					for ( size_t j = 0; j < remaining.size(); j++ )
					{
						double uo = pot->g2g(spc->p, *g[moved[i]], *g[remaining[j]]);
						double un = pot->g2g(spc->trial, *g[moved[i]], *g[remaining[j]]);
						double udiff = un - uo;
						if ( slump() < (1. - std::exp(-udiff)))
						{
							moved.push_back(remaining[j]);
							remaining.erase(remaining.begin() + j);
							j--;
						}
					}
					g[moved[i]]->accept(*spc);
				}

				if ( skipEnergyUpdate == false )
#pragma omp parallel for reduction (+:du) schedule (dynamic)
					for ( size_t i = 0; i < g.size() - 1; i++ )
						for ( size_t j = i + 1; j < g.size(); j++ )
							du += pot->g2g(spc->p, *g[i], *g[j]);

				base::alternateReturnEnergy = du;
				movefrac += double(moved.size()) / (moved.size() + remaining.size());

				assert(moved.size() >= 1);
				assert(spc->groupList().size() == moved.size() + remaining.size());
			}

		/**
		 * @brief Crank shaft move of linear polymers
		 *
		 * This will perform a crank shaft move of a linear polymer molecule.
		 * Two monomers are picked at random and a rotation axis is drawn
		 * between them. The particles in between are rotated around that
		 * axis. By setting `minlen` and `maxlen` one can control the maximum
		 * number particles to rotate. For example, for a crankshaft
		 * move spanning only one bond, set `minlen=maxlen=1`.
		 * The behavoir for branched molecules is currently undefined.
		 *
		 * ![Figure: Various polymer moves.](polymerdisplacements.jpg)
		 *
		 * @date Lund 2012
		 */
		template<class Tspace>
			class CrankShaft : public Movebase<Tspace>
		{
			private:
				typedef Movebase<Tspace> base;
				void _test( UnitTest & ) override;
				void _trialMove() override;
				void _acceptMove() override;
				void _rejectMove() override;
				double _energyChange() override;
				string _info() override;
				virtual bool findParticles(); //!< This will set the end points and find particles to rotate
			protected:
				std::map<int, int> _minlen, _maxlen;
				using base::spc;
				using base::pot;
				using base::w;
				Group *gPtr;       //!< Pointer to group where move is to be performed. Set by setGroup().
				double dp;         //!< Rotational displacement parameter
				double angle;      //!< Current rotation angle
				vector<int> index; //!< Index of particles to rotate
				//Geometry::VectorRotate vrot;
				Geometry::QuaternionRotate vrot;
				AcceptanceMap<string> accmap;
			public:
				CrankShaft( Energy::Energybase<Tspace> &, Tspace &, Tmjson & );
				virtual ~CrankShaft();
				void setGroup( Group & ); //!< Select Group to of the polymer to move
				int minlen;            //!< Minimum number of particles to rotate (default = 1)
				int maxlen;            //!< Maximin number of particles to rotate (default = 10)
		};

		/**
		 * The json entry is searched for:
		 *
		 * Key      | Description
		 * -------- | -------------------------------------------
		 * `minlen` | Minimum number of particles to rotate (default: 1)
		 * `maxlen` | Maximum number of particles to rotate (default: 4)
		 * `dp`     | Rotational displacement parameter (radians)
		 */
		template<class Tspace>
			CrankShaft<Tspace>::CrankShaft( Energy::Energybase<Tspace> &e,
					Tspace &s, Tmjson &j ) : base(e, s)
		{
			base::title = "CrankShaft";
			w = 30;
			gPtr = nullptr;

			auto m = j;
			base::fillMolList(m);
			for ( auto &i : this->mollist )
			{
				string name = spc->molList()[i.first].name;
				i.second.dp1 = m[name].at("dp");
				_minlen[i.first] = m[name].at("minlen");
				_maxlen[i.first] = m[name].at("maxlen");
			}
		}

		template<class Tspace>
			CrankShaft<Tspace>::~CrankShaft() {}

		template<class Tspace>
			void CrankShaft<Tspace>::_trialMove()
			{

				if ( !this->mollist.empty())
				{
					auto gvec = spc->findMolecules(this->currentMolId);
					assert(!gvec.empty());
					gPtr = *slump.element(gvec.begin(), gvec.end());
					assert(!gPtr->empty());
					dp = this->mollist[this->currentMolId].dp1;
					minlen = _minlen[this->currentMolId];
					maxlen = _maxlen[this->currentMolId];
				}

				assert(gPtr != nullptr && "No group to perform crankshaft on.");
				if ( gPtr->size() < 3 )
					return;
				index.clear();   // clear previous particle list to rotate
				findParticles();
				assert(!index.empty() && "No particles to rotate.");
				for ( auto i : index )
					spc->trial[i] = vrot(spc->p[i]); // (boundaries are accounted for)
				gPtr->cm_trial = Geometry::massCenter(spc->geo, spc->trial, *gPtr);

				int g_index = spc->findIndex(gPtr);

				// register index of the moved group and all it's moved particles
				for(auto i : index)
					base::change.mvGroup[g_index].push_back(i);
			}

		template<class Tspace>
			void CrankShaft<Tspace>::_acceptMove()
			{
				double msq = 0;
				for ( auto i : index )
				{
					msq += spc->geo.sqdist(spc->p[i], spc->trial[i]);
					spc->p[i] = spc->trial[i];
				}
				accmap.accept(gPtr->name, msq);
				gPtr->cm = gPtr->cm_trial;
			}

		template<class Tspace>
			void CrankShaft<Tspace>::_rejectMove()
			{
				accmap.reject(gPtr->name);
				for ( auto i : index )
					spc->trial[i] = spc->p[i];
				gPtr->cm_trial = gPtr->cm;
			}

		/**
		 * @todo g_internal is not really needed - index<->g would be faster
		 */
		template<class Tspace>
			double CrankShaft<Tspace>::_energyChange()
			{
				return Energy::energyChange(*spc, *base::pot, base::change);
			}

		/**
		 * This will define the particles to be rotated (stored in index vector) and
		 * also set the axis to rotate around, defined by two points.
		 */
		template<class Tspace>
			bool CrankShaft<Tspace>::findParticles()
			{
				assert(minlen <= gPtr->size() - 2 && "Minlen too big for molecule!");

				int beg, end, len;
				do
				{
					beg = gPtr->random();             // generate random vector to
					end = gPtr->random();             // rotate around
					len = std::abs(beg - end) - 1;    // number of particles between end points
				}
				while ( len < minlen || len > maxlen );

				angle = dp * slump.half();  // random angle
				vrot.setAxis(spc->geo, spc->p[beg], spc->p[end], angle);

				index.clear();
				if ( beg > end )
					std::swap(beg, end);
				for ( int i = beg + 1; i < end; i++ )
					index.push_back(i);             // store particle index to rotate
				assert(index.size() == size_t(len));

				return true;
			}

		template<class Tspace>
			void CrankShaft<Tspace>::setGroup( Group &g ) { gPtr = &g; }

		template<class Tspace>
			string CrankShaft<Tspace>::_info()
			{
				using namespace textio;
				std::ostringstream o;
				o << pad(SUB, w, "Displacement parameter") << dp << endl
					<< pad(SUB, w, "Min/max length to move") << minlen << " " << maxlen << endl;
				if ( base::cnt > 0 )
					o << accmap.info();
				return o.str();
			}

		template<class Tspace>
			void CrankShaft<Tspace>::_test( UnitTest &t )
			{
				accmap._test(t, textio::trim(base::title));
			}

		/**
		 * @brief Pivot move for linear polymers
		 *
		 * This will perform a pivot rotation of a linear polymer by the following steps:
		 *
		 * - Select rotation axis by two random monomers, spanning `minlen` to `maxlen` bonds
		 * - Rotate monomers before or after end points of the above axis
		 *
		 * ![Figure: Various polymer moves.](polymerdisplacements.jpg)
		 *
		 * @date Asljunga 2012
		 */
		template<class Tspace>
			class Pivot : public CrankShaft<Tspace>
		{
			protected:
				typedef CrankShaft<Tspace> base;
				using base::index;
				using base::gPtr;
				using base::spc;
				bool findParticles();
			public:
				Pivot( Energy::Energybase<Tspace> &, Tspace &, Tmjson & );
		};

		template<class Tspace>
			Pivot<Tspace>::Pivot( Energy::Energybase<Tspace> &e, Tspace &s, Tmjson &j ) : base(e, s, j)
		{
			base::title = "Polymer Pivot Move";
			base::minlen = 1; // minimum bond length to rotate around
		}

		template<class Tspace>
			bool Pivot<Tspace>::findParticles()
			{
				int beg(0), end(0), len;
				index.clear();
				while ( index.empty())
				{
					do
					{
						beg = gPtr->random(); // define the
						end = gPtr->random(); // axis to rotate around
						len = std::abs(beg - end);
					}
					while ( len < base::minlen || len > base::maxlen );

					if ( slump.half() > 0 )
						for ( int i = end + 1; i <= gPtr->back(); i++ )
							index.push_back(i);
					else
						for ( int i = gPtr->front(); i < end; i++ )
							index.push_back(i);
				}
				base::angle = base::dp * slump.half();
				base::vrot.setAxis(spc->geo, spc->p[beg], spc->p[end], base::angle);
				return true;
			}

		/**
		 * @brief Reptation move for linear polymers
		 *
		 * This will perform a reptation move of a linear, non-uniform polymer chain.
		 * During construction, the input is read from the `moves/reptate` section:
		 * `:
		 *
		 * Key           | Description
		 * :------------ | :---------------------------------------------------------------------------
		 * `prob`        | Probability to perform a move (defaults=1)
		 * `bondlength`  | The bond length while moving head groups. Use -1 to use existing bondlength.
		 *
		 * @date Lund 2012
		 */
		template<class Tspace>
			class Reptation : public Movebase<Tspace>
		{
			private:
				typedef Movebase<Tspace> base;
				AcceptanceMap<string> accmap;
				void _test( UnitTest & ) override;
				void _trialMove() override;
				void _acceptMove() override;
				void _rejectMove() override;
				double _energyChange() override;
				string _info() override;
				Group *gPtr;
				double bondlength; //!< Reptation length used when generating new head group position
			protected:
				using base::pot;
				using base::spc;
			public:
				Reptation( Energy::Energybase<Tspace> &, Tspace &, Tmjson & );
				void setGroup( Group & ); //!< Select Group to move
		};

		template<class Tspace>
			Reptation<Tspace>::Reptation( Energy::Energybase<Tspace> &e,
					Tspace &s, Tmjson &j ) : base(e, s)
		{

			base::title = "Linear Polymer Reptation";
			gPtr = nullptr;

			auto m = j;
			base::fillMolList(m);         // find molecules to be moved
			for ( auto &i : this->mollist )
			{ // loop over molecules to be moved
				string molname = spc->molList()[i.first].name;
				i.second.dp1 = m[molname]["bondlength"] | -1.0;
			}
		}

		template<class Tspace>
			void Reptation<Tspace>::_test( UnitTest &t )
			{
				accmap._test(t, textio::trim(base::title));
			}

		template<class Tspace>
			void Reptation<Tspace>::_trialMove()
			{

				gPtr = nullptr;
				if ( !this->mollist.empty())
				{
					auto gvec = spc->findMolecules(this->currentMolId);
					if ( !gvec.empty())
					{
						gPtr = *slump.element(gvec.begin(), gvec.end());
						bondlength = this->mollist[this->currentMolId].dp1;
					}
				}

				if ( gPtr == nullptr )
					throw std::runtime_error("Molecule " + gPtr->name + " not found in space");
				if ( gPtr->size() < 2 )
					throw std::runtime_error("Molecule " + gPtr->name + " too short for reptation.");

				int first, second; // "first" is end point, "second" is the neighbor
				if ( slump.half() > 0 )
				{
					first = gPtr->front();
					second = first + 1;
				}
				else
				{
					first = gPtr->back();
					second = first - 1;
				}

				double bond;
				if ( bondlength > 0 )
					bond = bondlength;
				else
					bond = spc->geo.dist(spc->p[first], spc->p[second]); // bond length of first or last particle

				// shift particles up or down
				for ( int i = gPtr->front(); i < gPtr->back(); i++ )
					if ( first < second )
						spc->trial[i + 1] = Point(spc->p[i]);
					else
						spc->trial[i] = Point(spc->p[i + 1]);

				// generate new position for end point ("first")
				Point u;
				u.ranunit(slump);                          // generate random unit vector
				spc->trial[first].translate(spc->geo, u * bond); // trans. 1st w. scaled unit vector
				assert(std::fabs(spc->geo.dist(spc->p[first], spc->trial[first]) - bond) < 1e-7);

				for ( auto i : *gPtr )
					spc->geo.boundary(spc->trial[i]);  // respect boundary conditions

				gPtr->cm_trial = Geometry::massCenter(spc->geo, spc->trial, *gPtr);
			}

		template<class Tspace>
			void Reptation<Tspace>::_acceptMove()
			{
				accmap.accept(gPtr->name, spc->geo.sqdist(gPtr->cm, gPtr->cm_trial));
				gPtr->accept(*spc);
			}

		template<class Tspace>
			void Reptation<Tspace>::_rejectMove()
			{
				accmap.reject(gPtr->name);
				gPtr->undo(*spc);
			}

		template<class Tspace>
			double Reptation<Tspace>::_energyChange()
			{
				for ( auto i : *gPtr )
					if ( spc->geo.collision(spc->trial[i], spc->trial[i].radius, Geometry::Geometrybase::BOUNDARY))
						return pc::infty;

				double unew = pot->g_external(spc->trial, *gPtr) + pot->g_internal(spc->trial, *gPtr);
				if ( unew == pc::infty )
					return pc::infty;       // early rejection
				double uold = pot->g_external(spc->p, *gPtr) + pot->g_internal(spc->p, *gPtr);

				for ( auto g : spc->groupList())
				{
					if ( g != gPtr )
					{
						unew += pot->g2g(spc->trial, *g, *gPtr);
						if ( unew == pc::infty )
							return pc::infty;   // early rejection
						uold += pot->g2g(spc->p, *g, *gPtr);
					}
				}
				return unew - uold;
			}

		template<class Tspace>
			string Reptation<Tspace>::_info()
			{
				using namespace textio;
				std::ostringstream o;
				o << pad(SUB, base::w, "Bondlength") << bondlength << _angstrom + " (-1 = automatic)\n";
				if ( base::cnt > 0 )
					o << accmap.info();
				return o.str();
			}

		/**
		 * @brief Isobaric volume move
		 *
		 * @details This class will perform a volume displacement and scale atomic
		 * as well as molecular groups as long as these are known to Space -
		 * see Space.enroll().
		 * The json object is scanned for the following keys in `moves/isobaric`:
		 *
		 * Key     | Description
		 * :-------| :-----------------------------
		 * `dV`    | Volume displacement parameter
		 * `P`     | Pressure [mM]
		 * `prob`  | Runfraction [default=1]
		 *
		 * Note that new volumes are generated according to
		 * \f$ V^{\prime} = \exp\left ( \log V \pm \delta dp \right ) \f$
		 * where \f$\delta\f$ is a random number between zero and one half.
		 */
		template<class Tspace>
			class Isobaric : public Movebase<Tspace>
		{
			private:
				typedef Movebase<Tspace> base;
			protected:
				string _info() override;
				void _test( UnitTest & ) override;
				void _trialMove() override;
				void _acceptMove() override;
				void _rejectMove() override;
				template<class Tpvec> double _energy( const Tpvec & );
				double _energyChange() override;
				using base::spc;
				using base::pot;
				using base::w;
				using base::change;
				double P; //!< Pressure
				double dp; //!< Volume displacement parameter
				double oldval;
				double newval;
				Point oldlen;
				Point newlen;
				Average<double> msd;       //!< Mean squared volume displacement
				Average<double> val;          //!< Average volume
				Average<double> rval;         //!< Average 1/volume
			public:
				template<typename Tenergy>
					Isobaric( Tenergy &, Tspace &, Tmjson & );
		};

		template<class Tspace>
			template<class Tenergy> Isobaric<Tspace>::Isobaric(
					Tenergy &e, Tspace &s, Tmjson &j ) : base(e, s)
			{

				this->title = "Isobaric Volume Fluctuations";
				this->w = 30;
				dp = j.at("dp");
				P = j.at("pressure").get<double>() * 1.0_mM;
				base::runfraction = j.value("prob", 1.0);
				if ( dp < 1e-6 )
					base::runfraction = 0;

				auto t = e.tuple(); // tuple w. pointers to all energy terms
				auto ptr = TupleFindType::get<Energy::ExternalPressure<Tspace> *>(t);
				if ( ptr != nullptr )
					(*ptr)->setPressure(P);
				else
					throw std::runtime_error(base::title+": pressure term required in hamiltonian");
				//auto ptr = e.template get<Energy::ExternalPressure<Tspace>>();
				//if ( ptr != nullptr )
				//    ptr->setPressure(P);
				//else
				//    throw std::runtime_error(base::title+": pressure term required in hamiltonian");
			}

		template<class Tspace>
			string Isobaric<Tspace>::_info()
			{
				using namespace textio;
				std::ostringstream o;
				int N, Natom = 0, Nmol = 0;
				for ( auto g : spc->groupList())
					if ( g->isAtomic())
						Natom += g->size();
					else
						Nmol += g->numMolecules();
				N = Natom + Nmol;
				o << pad(SUB, w, "Displacement parameter") << dp << endl
					<< pad(SUB, w, "Number of molecules")
					<< N << " (" << Nmol << " molecular + " << Natom << " atomic)\n"
					<< pad(SUB, w, "Pressure")
					<< P / 1.0_mM << " mM = " << P / 1.0_Pa << " Pa = " << P / 1.0_atm << " atm\n"
					<< pad(SUB, w, "Temperature") << pc::T() << " K\n";
				if ( base::cnt > 0 )
				{
					char l = 14;
					o << pad(SUB, w, "Mean displacement")
						<< cuberoot + rootof + bracket("dp" + squared)
						<< " = " << pow(msd.avg(), 1 / 6.) << _angstrom << endl
						<< pad(SUB, w, "Osmotic coefficient") << P / (N * rval.avg()) << endl
						<< endl
						<< indent(SUBSUB) << std::right << setw(10) << ""
						<< setw(l + 5) << bracket("V")
						<< setw(l + 8) << cuberoot + bracket("V")
						<< setw(l + 8) << bracket("1/V")
						<< setw(l + 8) << bracket("N/V") << endl
						<< indent(SUB) << setw(10) << "Averages"
						<< setw(l) << val.avg() << _angstrom + cubed
						<< setw(l) << std::cbrt(val.avg()) << _angstrom
						<< setw(l) << rval.avg() << " 1/" + _angstrom + cubed
						<< setw(l) << N * rval.avg() / 1.0_mM << " mM\n";
				}
				return o.str();
			}

		template<class Tspace>
			void Isobaric<Tspace>::_test( UnitTest &t )
			{
				string sec = textio::trim(base::title);
				t(sec + "_averageSideLength", std::cbrt(val.avg()));
				t(sec + "_MSQDisplacement", pow(msd.avg(), 1 / 6.));
			}

		template<class Tspace>
			void Isobaric<Tspace>::_trialMove()
			{
				assert(spc->groupList().size() > 0
						&& "Space has empty group vector - NPT move not possible.");
				oldval = spc->geo.getVolume();
				oldlen = newlen = spc->geo.len;
				newval = std::exp(std::log(oldval) + slump.half() * dp);
				//newval = oldval*std::exp( slump.half()*dp ); // Is this not more simple?
				Point s = Point(1, 1, 1);
				double xyz = cbrt(newval / oldval);
				double xy = sqrt(newval / oldval);
				newlen.scale(spc->geo, s, xyz, xy);
				for ( auto g : spc->groupList())
				{
					g->setMassCenter(*spc);
					g->scale(*spc, s, xyz, xy); // scale trial coordinates to new volume
				}

				spc->geo_trial.setlen(newlen);

				// register all moved groups in change object
				int i = 0;
				for ( auto gPtr : spc->groupList())
				{
					if (gPtr->isAtomic())
					{
						change.mvGroup[i].resize(gPtr->size());
						std::iota( change.mvGroup[i].begin(), change.mvGroup[i].end(), gPtr->front());
						assert(gPtr->size() == int(change.mvGroup[i].size()));
						assert(gPtr->front() == change.mvGroup[i].front());
						assert(gPtr->back() == change.mvGroup[i].back());
					}
					else
						change.mvGroup[i].clear();
					i++;
				}
				change.geometryChange = true;
				change.dV = newval - oldval;
			}

		template<class Tspace>
			void Isobaric<Tspace>::_acceptMove()
			{
				val += newval;
				msd += pow(oldval - newval, 2);
				rval += 1. / newval;
				spc->geo.setlen(newlen);
				pot->setSpace(*spc);
				for ( auto g : spc->groupList())
					g->accept(*spc);
			}

		template<class Tspace>
			void Isobaric<Tspace>::_rejectMove()
			{
				msd += 0;
				val += oldval;
				rval += 1. / oldval;
				spc->geo.setlen(oldlen);
				spc->geo_trial = spc->geo;
				pot->setSpace(*spc);
				for ( auto g : spc->groupList())
					g->undo(*spc);
			}

		/**
		 * This will calculate the total energy of the configuration
		 * associated with the current Hamiltonian volume
		 */
		template<class Tspace>
			template<class Tpvec>
			double Isobaric<Tspace>::_energy( const Tpvec &p )
			{
				double u = 0;
				if ( dp < 1e-6 )
					return u;
				size_t n = spc->groupList().size();  // number of groups
				for ( size_t i = 0; i < n - 1; ++i )      // group-group
					for ( size_t j = i + 1; j < n; ++j )
						u += pot->g2g(p, *spc->groupList()[i], *spc->groupList()[j]);

				for ( auto g : spc->groupList())
				{
					u += pot->g_external(p, *g);
					if ( g->numMolecules() > 1 )
						u += pot->g_internal(p, *g);
				}
				return u + pot->external(p);
			}

		template<class Tspace>
			double Isobaric<Tspace>::_energyChange()
			{
				return Energy::energyChange(*spc, *pot, change);
			}

		/**
		 * @brief Isochoric scaling move in Cuboid geometry
		 *
		 * @details This class will expand/shrink along the z-axis
		 * and shrink/expand in the xy-plane atomic as well as molecular groups
		 * as long as these are known to Space - see Space.enroll().
		 * The json object class is scanned for the following keys:
		 *
		 * Key                | Description
		 * :----------------- | :-----------------------------
		 * `nvt_dz`           | Length displacement parameter
		 * `nvt_runfraction`  | Runfraction [default=1]
		 *
		 */
		template<class Tspace>
		   class Isochoric : public Isobaric<Tspace>
		{
			protected:
				typedef Isobaric<Tspace> base;
				using base::spc;
				using base::pot;
				using base::w;
				using base::dp;
				using base::oldval;
				using base::newval;
				using base::oldlen;
				using base::newlen;
				using base::msd;
				using base::val;
				void _trialMove();
				string _info();
			public:
				template<typename Tenergy>
					Isochoric( Tenergy &, Tspace &, Tmjson & );
		};

		template<class Tspace>
			template<class Tenergy> Isochoric<Tspace>::Isochoric( Tenergy &e, Tspace &s, Tmjson &j ) : base(e, s, j)
		{
			this->title = "Isochoric Side Lengths Fluctuations";
			this->w = 30;
			dp = j.at("dp");
			base::runfraction = j.value("prob", 1.0);
			if ( dp < 1e-6 )
				base::runfraction = 0;
		}

		template<class Tspace>
			string Isochoric<Tspace>::_info()
			{
				using namespace textio;
				std::ostringstream o;
				o << pad(SUB, w, "Displacement parameter") << dp << endl
					<< pad(SUB, w, "Temperature") << pc::T() << " K\n";
				if ( base::cnt > 0 )
				{
					char l = 14;
					o << pad(SUB, w, "Mean displacement")
						<< rootof + bracket("dz" + squared)
						<< " = " << pow(msd.avg(), 1 / 2.) << _angstrom << endl
						<< endl
						<< indent(SUBSUB) << std::right << setw(10) << ""
						<< setw(l + 5) << bracket("Lz") << endl
						<< indent(SUB) << setw(10) << "Averages"
						<< setw(l) << val.avg() << _angstrom + cubed;
				}
				return o.str();
			}

		template<class Tspace>
			void Isochoric<Tspace>::_trialMove()
			{
				assert(spc->groupList().size() > 0
						&& "Space has empty group vector - Isochoric scaling move not possible.");
				oldlen = spc->geo.len;
				newlen = oldlen;
				oldval = spc->geo.len.z();
				newval = std::exp(std::log(oldval) + slump.half() * dp);
				//newval = oldval+ slump.half()*dp;
				Point s;
				s.z() = newval / oldval;
				s.x() = s.y() = 1 / std::sqrt(s.z());
				newlen.scale(spc->geo, s);
				for ( auto g : spc->groupList())
				{
					g->scale(*spc, s); // scale trial coordinates to new coordinates
				}
			}

		/**
		 * @brief Grand Canonical insertion of arbitrary M:X salt pairs
		 *
		 * This will do GC moves of salt pairs, automatically combined
		 * from their valencies. JSON input:
		 *
		 * ~~~~
		 * "moves" : {
		 *   "atomgc" : { "molecule":"mysalt", "prob":1.0 }
		 * }
		 * ~~~~
		 *
		 * where `mysalt` must be an atomic molecule. Only atom types with
		 * non-zero activities will be considered.
		 *
		 * @date Lund 2010-2011
		 * @warning Untested for asymmetric salt in this branch
		 */
		template<class Tspace>
			class GrandCanonicalSalt : public Movebase<Tspace>
		{
			protected:
				typedef Movebase<Tspace> base;
				typedef typename Tspace::ParticleType Tparticle;
				typedef typename Tspace::ParticleVector Tpvec;
				typedef typename Tparticle::Tid Tid;
				using base::pot;
				using base::spc;
				using base::w;
				string _info() override;
				Tmjson _json() override;
				void _trialMove() override;
				void _acceptMove() override;
				void _rejectMove() override;
				double _energyChange() override;
				void add( Group & );       // scan group for ions with non-zero activities

				struct ionprop
				{
					Tparticle p;
					double chempot;       // chemical potential log(1/A3)
					Average<double> rho;  // average density
				};

				std::map<Tid, ionprop> map;

				/** @brief Find random ion type in salt group */
				Tid randomAtomType() const {
					auto it = slump.element( map.begin(), map.end() );
					if (it==map.end())
						throw std::runtime_error("no ions could be found");
					return it->first;
				}

				void randomIonPair( Tid&, Tid& );  // Generate random ion pair
				Tpvec trial_insert;
				vector<int> trial_delete;
				Tid ida, idb;     // particle id's of current salt pair (a=cation, b=anion)

				Group *saltPtr;  // GC ions *must* be in this group
				int saltmolid;   // Molecular ID of salt

				// unit testing
				void _test( UnitTest &t ) override
				{
					string sec = textio::trim(base::title);
					for ( auto &m : map )
					{
						auto s = sec + "_" + atom[m.first].name;
						t(s + "_activity", atom[m.first].activity);
						t(s + "_conc", m.second.rho.avg() / pc::Nav / 1e-27);
					}
				}

			public:
				GrandCanonicalSalt( Energy::Energybase<Tspace> &, Tspace &, Tmjson & );
		};

		template<class Tspace>
			GrandCanonicalSalt<Tspace>::GrandCanonicalSalt(
					Energy::Energybase<Tspace> &e, Tspace &s, Tmjson &j ) : base(e, s)
			{

				base::title = "Grand Canonical Salt";
				base::useAlternativeReturnEnergy = true;
				base::runfraction = j.value("prob", 1.0);
				string saltname = j.at("molecule");

				auto v = spc->findMolecules(saltname);
				if ( v.empty())
				{ // insert if no atomic species found
					auto it = spc->molList().find(saltname);
					if ( it != spc->molList().end())
						saltPtr = spc->insert(it->id, it->getRandomConformation(spc->geo, spc->p));
				}
				else
				{
					if ( v.size() != 1 )
						throw std::runtime_error("Number of atomic GC groups must be exactly ONE.");
					if ( v.front()->isMolecular())
						throw std::runtime_error("Atomic GC group must be atomic.");
					saltPtr = v.front();
				}
				add(*saltPtr);
			}

		template<class Tspace>
			void GrandCanonicalSalt<Tspace>::add( Group &g )
			{
				saltmolid = g.molId;
				assert(g.isAtomic() && "Salt group must be atomic");
				for ( auto i : g )
				{
					auto id = spc->p[i].id;
					if ( atom[id].activity > 1e-10 && fabs(atom[id].charge) > 1e-10 )
					{
						map[id].p = atom[id];
						map[id].chempot = log(atom[id].activity * pc::Nav * 1e-27); // beta mu
					}
				}
			}

		template<class Tspace>
			void GrandCanonicalSalt<Tspace>::randomIonPair( Tid &id_cation, Tid &id_anion )
			{
				do
					id_anion = randomAtomType();
				while ( map[id_anion].p.charge >= 0 );
				do
					id_cation = randomAtomType();
				while ( map[id_cation].p.charge <= 0 );
				assert( id_cation != id_anion );
			}

		template<class Tspace>
			void GrandCanonicalSalt<Tspace>::_trialMove()
			{
				trial_insert.clear();
				trial_delete.clear();

				randomIonPair(ida, idb);

				assert(ida > 0 && idb > 0 &&
						"Ion pair id is zero (UNK). Is this really what you want?");

				size_t Na = (size_t) abs(map[idb].p.charge);
				size_t Nb = (size_t) abs(map[ida].p.charge);
				int ran = slump.range(0,1); 
				switch (ran)
				{
					case 0: // attempt to insert
						trial_insert.reserve(Na + Nb);
						do {
							trial_insert.push_back(map[ida].p);
							assert(map[ida].p.id==ida);
						}
						while ( --Na > 0 );
						do {
							trial_insert.push_back(map[idb].p);
							assert(map[idb].p.id==idb);
						}
						while ( --Nb > 0 );

						for ( auto &p : trial_insert ) //assign random positions
							spc->geo.randompos(p);
						break;

					case 1: // attempt to delete

						vector<int> vecA = spc->atomTrack[ida]; // vector of ida index
						vector<int> vecB = spc->atomTrack[idb]; // vector of idb index

						if ( vecA.size() < Na || vecB.size() < Nb )
							return; // abort - not enough particles to delete

						trial_delete.reserve(Na + Nb);

						while ( trial_delete.size() != Na )
						{
							assert(!vecA.empty());
							auto it = slump.element(vecA.begin(), vecA.end()); // random ida particle
							int i = *it;
							vecA.erase(it);

							assert(ida == spc->p[i].id && "id mismatch");

							trial_delete.push_back(i);
						}
						while ( trial_delete.size() != Na + Nb )
						{
							assert(!vecB.empty());
							auto it = slump.element(vecB.begin(), vecB.end()); // random ida particle
							int i = *it;
							vecB.erase(it);

							assert(idb == spc->p[i].id && "id mismatch" );

							trial_delete.push_back(i);
						}
						assert( trial_delete.size() == Na + Nb);
						break;
				}
			}

		template<class Tspace>
			double GrandCanonicalSalt<Tspace>::_energyChange()
			{
				int Na = 0, Nb = 0;            // number of added or deleted ions
				double idfactor = 1;
				double uold = 0, unew = 0, V = spc->geo.getVolume();
				double potold = 0, potnew = 0; // energy change due to interactions

				if ( trial_insert.size() > 0 )
				{
					for ( auto &t : trial_insert )     // count added ions
						if ( t.id == map[ida].p.id )
							Na++;
						else
							Nb++;
					for ( int n = 0; n < Na; n++ )
						idfactor *= (spc->atomTrack[ida].size() + 1 + n) / V;
					for ( int n = 0; n < Nb; n++ )
						idfactor *= (spc->atomTrack[idb].size() + 1 + n) / V;

					unew = log(idfactor) - Na * map[ida].chempot - Nb * map[idb].chempot;

					potnew += pot->v2v(spc->p, trial_insert);
					for ( auto i = trial_insert.begin(); i != trial_insert.end() - 1; i++ )
						for ( auto j = i + 1; j != trial_insert.end(); j++ )
							potnew += pot->p2p(*i, *j);

					for ( auto &i : trial_insert )
						potnew += pot->p_external(i);

					unew += potnew;
				}
				else if ( trial_delete.size() > 0 )
				{
					for ( auto i : trial_delete )
					{
						if ( spc->p[i].id == map[ida].p.id )
							Na++;
						else if ( spc->p[i].id == map[idb].p.id )
							Nb++;
					}
					for ( int n = 0; n < Na; n++ )
						idfactor *= (spc->atomTrack[ida].size() - Na + 1 + n) / V;
					for ( int n = 0; n < Nb; n++ )
						idfactor *= (spc->atomTrack[idb].size() - Nb + 1 + n) / V;

					unew = -log(idfactor) + Na * map[ida].chempot + Nb * map[idb].chempot;

					for ( auto &i : trial_delete )
						potold += pot->i_total(spc->p, i);
					for ( auto i = trial_delete.begin(); i != trial_delete.end() - 1; i++ )
						for ( auto j = i + 1; j != trial_delete.end(); j++ )
							potold -= pot->i2i(spc->p, *i, *j);
					uold += potold;
				}

				base::alternateReturnEnergy = potnew - potold; // track only pot. energy
				return unew - uold;
			}

		template<class Tspace>
			void GrandCanonicalSalt<Tspace>::_acceptMove()
			{
				int Nold = 0;
				auto v = spc->findMolecules(saltmolid);
				if (!v.empty()) {
					assert( v.front()==saltPtr );
					Nold = v.front()->size();
				} else
					saltPtr=nullptr;

				if ( !trial_insert.empty()) {
					saltPtr = spc->insert( saltmolid, trial_insert );
					assert( saltPtr!=nullptr );
					assert( saltPtr->size() == Nold + (int)trial_insert.size() );
				}

				if ( !trial_delete.empty()) {
					assert(saltPtr!=nullptr);
					std::sort(trial_delete.rbegin(), trial_delete.rend()); //reverse sort
					for ( auto i : trial_delete )
						spc->erase(i);
				}

				double V = spc->geo.getVolume();
				map[ida].rho += spc->atomTrack[ida].size() / V;
				map[idb].rho += spc->atomTrack[idb].size() / V;
			}

		template<class Tspace>
			void GrandCanonicalSalt<Tspace>::_rejectMove()
			{
				double V = spc->geo.getVolume();
				map[ida].rho += spc->atomTrack[ida].size() / V;
				map[idb].rho += spc->atomTrack[idb].size() / V;
			}

		template<class Tspace>
			string GrandCanonicalSalt<Tspace>::_info()
			{
				char s = 10;
				using namespace textio;
				std::ostringstream o;
				o << pad(SUB, w, "Number of GC species") << endl << endl;
				o << setw(4) << "" << std::left
					<< setw(s) << "Ion" << setw(s) << "activity"
					<< setw(s + 4) << bracket("c/M") << setw(s + 6) << bracket(gamma + pm)
					<< setw(s + 4) << bracket("N") << "\n";
				for ( auto &m : map )
				{
					Tid id = m.first;
					o.precision(5);
					o << setw(4) << "" << setw(s) << atom[id].name
						<< setw(s) << atom[id].activity << setw(s) << m.second.rho.avg() / pc::Nav / 1e-27
						<< setw(s) << atom[id].activity / (m.second.rho.avg() / pc::Nav / 1e-27)
						<< setw(s) << m.second.rho.avg() * spc->geo.getVolume()
						<< "\n";
				}
				return o.str();
			}

		template<class Tspace>
			Tmjson GrandCanonicalSalt<Tspace>::_json()
			{
				Tmjson js;
				if ( base::cnt > 0 )
				{
					auto &j = js[base::title];
					for ( auto &m : map )
					{ // loop over GC species
						Tid id = m.first;
						j["atoms"][atom[id].name] = {
							{"activity", atom[id].activity},
							{"molarity", m.second.rho.avg() / pc::Nav / 1e-27},
							{"gamma", atom[id].activity / (m.second.rho.avg() / pc::Nav / 1e-27)},
							{"N", m.second.rho.avg() * spc->geo.getVolume()}
						};
					}
				}
				return js;
			}

		/**
		 * @brief Grand Canonical Titration derived from Grand Canonical Salt
		 * @date Lund 2015
		 *
		 * Input parameters:
		 *
		 *  Keyword      | Description
		 *  :----------- | :----------------------
		 *  `neutralize` | Neutralize system w. GC ions. Default: `true`
		 *  `avgfile`    | Save AAM/PQR file w. average charges at end of simulation (TODO)
		 *  `scale2int`  | When saving `avgfile`, scale charges to ensure integer net charge (default: `false`)
		 *  `processes`  | List of equilibrium processes, see `Energy::EquilibriumController`
		 *
		 * @todo: contains lots of redundant code from SwapMove, could inherit from there
		 * as well
		 */
		template<class Tspace>
			class GrandCanonicalTitration : public GrandCanonicalSalt<Tspace>
		{
			private:
				string avgfile;
				bool scale2int;   // if true, scale avg. charge to nearest int
			protected:
				typedef GrandCanonicalSalt<Tspace> base;
				typedef typename Tspace::ParticleType Tparticle;
				typedef typename Tspace::ParticleVector Tpvec;
				typedef typename Tparticle::Tid Tid;
				Energy::EquilibriumEnergy<Tspace> *eqpot;
				using base::spc;
				using base::pot;
				using base::w;
				void _trialMove() override;
				void _acceptMove() override;
				void _rejectMove() override;
				string _info() override;
				double _energyChange() override;

				void add( Group & ) {};       // scan group for ions with non-zero activities

				unsigned long int cnt_tit, cnt_salt, cnt_tit_acc, cnt_salt_acc;
				Tid pid;     // particle id's of current salt pair (a=cation, b=anion)
				int N, isite = -1;                    // switch with a built in message
				int k;
				bool protonation;          // if yes, the process shuld lead to protonation
				bool gcyes;

				std::map<int, Average<double> > accmap; //!< Site acceptance map
				std::map<int, std::map<int, Average<double> >> molCharge;

				void updateMolCharge( int pindex )
				{
					auto g = spc->findGroup(pindex);
					molCharge[g->molId][pindex - g->front()] += spc->p[pindex].charge;
				}

			public:
				template<class Tenergy>
					GrandCanonicalTitration( Tenergy &, Tspace &, Tmjson & );

				~GrandCanonicalTitration() { /* todo */ }

				template<class Tpvec>
					int findSites( Tpvec &p )
					{
						accmap.clear();
						return eqpot->findSites(p);
					}

				Tmjson infojson();

				/** @brief Copy average charges into particle vector */
				template<class Tpvec>
					void applyCharges( Tpvec &p )
					{
						for ( auto &g : spc->groupList()) // loop over all groups
							for ( auto &i : molCharge[g->molId] ) // loop over particles
								p[g->front() + i.first].charge = i.second;
					}
		};

		template<class Tspace>
			template<class Tenergy>

			GrandCanonicalTitration<Tspace>::GrandCanonicalTitration(
					Tenergy &e,
					Tspace &s,
					Tmjson &j ) : base(e, s, j)
		{

			base::title += " Titration";
			base::useAlternativeReturnEnergy = true;
			auto t = e.tuple();
			auto ptr = TupleFindType::get<Energy::EquilibriumEnergy<Tspace> *>(t);
			if ( ptr != nullptr )
				eqpot = *ptr;
			else
			{
				throw std::runtime_error("Error: `Energy::EquilibriumEnergy` required in Hamiltonian\
						for Grand Canonical Titration moves.");
			}
			eqpot->eq = Energy::EquilibriumController(j);
			findSites(spc->p);

			if ( eqpot->eq.sites.empty())
				std::cerr << "Warning: No processes found for `Move::SwapMove`.\n";

			cnt_tit = cnt_salt = cnt_tit_acc = cnt_salt_acc = 0;

			/* Sync particle charges with `AtomMap` */
			for ( auto i : eqpot->eq.sites )
				spc->trial[i].charge = spc->p[i].charge = atom[spc->p[i].id].charge;

			avgfile = j["avgfile"] | string();
			scale2int = j["scale2int"] | false;

			// neutralise system, if needed, using GC ions
			if ( j["neutralize"] | true )
			{
				double Z = netCharge(s.p, Group(0, s.p.size() - 1)); // total system charge
				if ( fabs(Z) > 1e-9 )
				{
					Tid id;
					double z = 0;
					int maxtry = 1000;
					cout << "# Neutralizing system with GC ions. Initial charge = "
						<< Z << "e." << endl;
					do
					{
						id = base::randomAtomType();
						z = atom[id].charge;
						if ( --maxtry == 0 )
							throw std::runtime_error(base::title+
									": no GC ions capable of neutralizing system found");
					}
					while ( Z * z > 0 || (fabs(fmod(Z, z)) > 1e-9) || atom[id].activity == 0 );

					int n = round(-Z / z);

					cout << "Type of neutralizing ion to insert = " << atom[id].name << endl;
					cout << "No. of neutralizing ions to insert = " << n << endl;

					assert(n > 0 && fabs(n * z + Z) < 1e-9);

					typename Tspace::ParticleType a;
					a = atom[id];
					for ( int i = 0; i < n; i++ )
					{
						s.geo.randompos(a);
						spc->insert(a, base::saltPtr->back());
					}
					Z = netCharge(s.p, Group(0, s.p.size() - 1));
					cout << "Final charge                       = " << Z << "e." << endl;
					assert(fabs(Z) < 1e-9);
					s.initTracker();
				}
			}
		}

		template<class Tspace>
			void GrandCanonicalTitration<Tspace>::_trialMove()
			{
				gcyes = false;
				int switcher = slump.range(0, 1);
				if ( eqpot->eq.number_of_sites() == 0 )
				{ // If no sites associated with processess
					gcyes = true, switcher = 0;            // fall back to plain gc
				}
				switch (switcher)
				{
					case 0:   // Go for the inheritance
						cnt_salt++;
						gcyes = true;
						base::_trialMove();
						break;
					case 1:   // Brand new deal
						cnt_tit++;
						base::trial_insert.clear(); // First some cleaning in the attic
						base::trial_delete.clear();
						do
						{              // Pick a monovalent ion
							pid = base::randomAtomType();
						}
						while ( atom[pid].charge * atom[pid].charge != 1 );
						if ( !eqpot->eq.sites.empty())
						{
							int i = slump.range(0, eqpot->eq.sites.size() - 1); // pick random site (local in *eq)
							isite = eqpot->eq.sites.at(i); // and corresponding particle (index in spc->p)
							do
							{
								k = slump.range(0, eqpot->eq.process.size() - 1);// pick random process..
							}
							while ( !eqpot->eq.process[k].one_of_us(this->spc->p[isite].id)); //that match particle isite

							eqpot->eq.process[k].swap(this->spc->trial[isite]); // change state and get intrinsic energy change
						}
						if ( !eqpot->eq.process[k].bound(this->spc->trial[isite].id))
						{// have action lead to deprotonation?
							protonation = false;
						}
						else
						{
							protonation = true;
						}
						N = -1;
						// The following section is hardcoded for monovalent salt
						if ( base::map[pid].p.charge > 0 )
						{  // Determine weather cat-/anion
							N = 0;            // N==0 cation, N==1 anion
						}
						else if ( base::map[pid].p.charge < 0 )
						{
							N = 1;
						}
						else
						{
							std::cerr << " Error, something fails !" << std::endl, exit(0);
						}
						int iIon = -1;
						if ( protonation == true )
						{
							if ( N == 0 )
							{ // Protonation and deletion of cation
								std::vector<int> dst;
								spc->atomTrack.find(pid, 1, dst); // find random index
								if (dst.size()==1)
									iIon = dst.front();
								else
									throw std::runtime_error("id not found");

								assert(pid == spc->p[iIon].id && "id mismatch");
								base::trial_delete.push_back(iIon);
							}
							else if ( N == 1 )
							{ // protonation and addition of anion
								base::trial_insert.push_back(base::map[pid].p);
								base::spc->geo.randompos(base::trial_insert[0]);
								assert(pid == base::trial_insert[0].id);
							}
							else
							{
								std::cerr << " Process error !" << std::endl;
								exit(1);
							}
						}
						else
						{
							if ( N == 0 )
							{ // Deprotonation and addition of cation
								base::trial_insert.push_back(base::map[pid].p);
								base::spc->geo.randompos(base::trial_insert[0]);
								assert(pid == base::trial_insert[0].id);
							}
							else if ( N == 1 )
							{ // Deprotonation and deletion of anion
								std::vector<int> dst;
								spc->atomTrack.find(pid, 1, dst); // find random index
								if (dst.size()==1)
									iIon = dst.front();
								else
									throw std::runtime_error("id not found");

								assert(pid == spc->p[iIon].id && "id mismatch");
								base::trial_delete.push_back(iIon);
							}
							else
							{
								std::cerr << " Process error !" << std::endl;
								exit(1);
							}
						}
						break;
				}
			};

		template<class Tspace>
			double GrandCanonicalTitration<Tspace>::_energyChange()
			{
				if ( gcyes == true ) // Go about the old habbit
					return base::_energyChange();
				double idfactor = 1;
				double uold = 0, unew = 0, V = spc->geo.getVolume();
				double potold = 0, potnew = 0; // energy change due to interactions
				double site_old = 0, salt_old = 0;
				double site_new = 0, salt_new = 0;
				potnew = pot->i_internal(spc->trial, isite);  // Intrinsic energies for new
				potold = pot->i_internal(spc->p, isite);      // and old state
				if ( protonation == true && N == 1 )
				{ // Protonate and ins. anion
					idfactor *= (spc->atomTrack[pid].size() + 1) / V;
					unew = log(idfactor) - base::map[pid].chempot;
					salt_new += pot->all2p(spc->trial, base::trial_insert[0]);

					site_new += pot->i2all(spc->trial, isite);

					site_old += pot->i2all(spc->p, isite);
				}
				if ( protonation == true && N == 0 )
				{ // Protonate and del. cat
					idfactor *= V / spc->atomTrack[pid].size();
					unew = log(idfactor) + base::map[pid].chempot;

					salt_old += pot->i2all(spc->p, base::trial_delete[0]);

					site_new += pot->i2all(spc->trial, isite);
					site_new -= pot->i2i(spc->trial, base::trial_delete[0], isite);

					site_old += pot->i2all(spc->p, isite);
					site_old -= pot->i2i(spc->p, base::trial_delete[0], isite); //Subtracted from previuous double count
				}
				if ( protonation == false && N == 0 )
				{ // Deprotonate and ins.
					idfactor *= (spc->atomTrack[pid].size() + 1) / V;   // cation
					unew = log(idfactor) - base::map[pid].chempot;
					salt_new += pot->all2p(spc->trial, base::trial_insert[0]);
					site_new += pot->i2all(spc->trial, isite);

					site_old += pot->i2all(spc->p, isite);
				}
				if ( protonation == false && N == 1 )
				{ // Deprotonate and del.
					idfactor *= V / spc->atomTrack[pid].size();       // anion
					unew = log(idfactor) + base::map[pid].chempot;

					salt_old += pot->i2all(spc->p, base::trial_delete[0]);

					site_new += pot->i2all(spc->trial, isite);
					site_new -= pot->i2i(spc->trial, base::trial_delete[0], isite);

					site_old += pot->i2all(spc->p, isite);
					site_old -= pot->i2i(spc->p, base::trial_delete[0], isite);
				}
				unew += potnew + salt_new + site_new;
				uold += potold + salt_old + site_old;
				potnew += salt_new + site_new;
				potold += salt_old + site_old;
				base::alternateReturnEnergy = potnew - potold; // track only pot. energy
				return unew - uold;
			};

		template<class Tspace>
			void GrandCanonicalTitration<Tspace>::_acceptMove()
			{
				if ( gcyes == true )
				{
					base::_acceptMove();
					cnt_salt_acc++;
				}
				else
				{
					assert(spc->p[isite].id != spc->trial[isite].id);
					spc->p[isite] = spc->trial[isite];
					if ( !base::trial_insert.empty())
					{
						base::saltPtr = spc->insert( base::saltmolid, base::trial_insert );
					}
					else if ( !base::trial_delete.empty())
					{
						std::sort(base::trial_delete.rbegin(), base::trial_delete.rend()); //reverse sort
						for ( auto i : base::trial_delete )
							spc->erase(i);
					}
					double V = spc->geo.getVolume();
					base::map[pid].rho += spc->atomTrack[pid].size() / V;
					accmap[isite] += 1;
					updateMolCharge(isite);
					cnt_tit_acc++;
					//spc->initTracker(); // only needed sine space is not used to insert/delete
				}
			};

		template<class Tspace>
			void GrandCanonicalTitration<Tspace>::_rejectMove()
			{
				if ( gcyes == true )
				{
					base::_rejectMove();
				}
				else
				{
					assert(spc->p[isite].id != spc->trial[isite].id);
					spc->trial[isite] = spc->p[isite];
					accmap[isite] += 0;
					updateMolCharge(isite);
					double V = spc->geo.getVolume();
					base::map[pid].rho += spc->atomTrack[pid].size() / V;
				}
			};

		template<class Tspace>
			string GrandCanonicalTitration<Tspace>::_info()
			{
				char s = 10;
				using namespace textio;
				std::ostringstream o;
				o << pad(SUB, w, "Number of GC species") << endl << endl;
				o << setw(4) << "" << std::left
					<< setw(s) << "Ion" << setw(s) << "activity"
					<< setw(s + 4) << bracket("c/M") << setw(s + 6) << bracket(gamma + pm)
					<< setw(s + 4) << bracket("N") << "\n";
				for ( auto &m : base::map )
				{
					Tid id = m.first;
					o.precision(5);
					o << setw(4) << "" << setw(s) << atom[id].name
						<< setw(s) << atom[id].activity << setw(s) << m.second.rho.avg() / pc::Nav / 1e-27
						<< setw(s) << atom[id].activity / (m.second.rho.avg() / pc::Nav / 1e-27)
						<< setw(s) << m.second.rho.avg() * spc->geo.getVolume()
						<< "\n";
				}
				for ( auto &m : molCharge )
				{ // loop over molecules

					int molid = m.first;
					auto g = spc->randomMol(molid); // random molecule

					if ( g != nullptr )
					{

						o << "\n" << indent(SUB) << "Molecule: " << spc->molList()[molid].name << "\n\n"
							<< std::left << "    " << setw(8) << "index" << setw(12) << "name"
							<< setw(12) << "Z" << "\n";

						for ( auto &i : m.second )
						{ // loop over atoms in molecule
							int j = g->front() + i.first; // particle index
							o << "    " << setw(8) << i.first
								<< setw(12) << atom[spc->p[j].id].name
								<< setw(12) << i.second << "\n";
						}
					}
				}
				std::ofstream f(textio::prefix + "gctit-output.json");
				f << setw(4) << infojson() << "\n";
				return o.str();
			}

		/** @brief Create JSON object with info **/
		template<class Tspace>
			Tmjson GrandCanonicalTitration<Tspace>::infojson()
			{
				Tmjson js;
				for ( auto &m : molCharge )
				{ // loop over molecules
					int molid = m.first;
					auto g = spc->randomMol(molid); // random molecule
					if ( g != nullptr )
					{
						string molname = spc->molList()[molid].name;
						for ( auto &i : m.second )
						{ // loop over particle index (starting from zero)
							int j = g->front() + i.first; // absolute particle index
							js[molname]["index"].push_back(i.first);
							js[molname]["charge"].push_back(i.second.avg());
							js[molname]["resname"].push_back(atom[spc->p[j].id].name);
						}
					}
				}
				return js;
			}

#ifdef ENABLE_MPI
		/**
		 * @brief Class for parallel tempering (aka replica exchange) using MPI
		 *
		 * This will perform replica exchange moves by the following steps:
		 *
		 * -# Randomly find an exchange partner with rank above/under current rank
		 * -# Exchange full particle configuration with partner
		 * -# Calculate energy change using Energy::systemEnergy. Note that this
		 *    energy function can be replaced by setting the `ParallelTempering::usys`
		 *    variable to another function with the same signature (functor wrapper).
		 * -# Send/receive energy change to/from partner
		 * -# Accept or reject based on *total* energy change
		 *
		 * Although not completely correct, the recommended way of performing a temper move
		 * is to do `N` Monte Carlo passes with regular moves and then do a tempering move.
		 * This is because the MPI nodes must be in sync and if you have a system where
		 * the random number generator calls are influenced by the Hamiltonian we could
		 * end up in a deadlock.
		 *
		 * @date Lund 2012
		 */
		template<class Tspace>
			class ParallelTempering : public Movebase<Tspace> {
				private:
					typedef typename Tspace::ParticleVector Tpvec;
					typedef Movebase<Tspace> base;
					using base::pot;
					using base::spc;
					using base::w;
					using base::runfraction;
					using base::mpiPtr;
					enum extradata {VOLUME=0};    //!< Structure of extra data to send
					typedef std::map<string, Average<double> > map_type;
					map_type accmap;              //!< Acceptance map
					int partner;                  //!< Exchange replica (partner)
					virtual void findPartner();   //!< Find replica to exchange with
					bool goodPartner();           //!< Is partned valid?
					double exchangeEnergy(double);//!< Exchange energy with partner
					string id();                  //!< Unique string to identify set of partners

					double currentEnergy;         //!< Energy of configuration before move (uold)
					bool haveCurrentEnergy;       //!< True if currentEnergy has been set

					string _info() override;
					void _trialMove() override;
					void _acceptMove() override;
					void _rejectMove() override;
					double _energyChange() override;
					//std::ofstream temperPath;

					Faunus::MPI::FloatTransmitter ft;   //!< Class for transmitting floats over MPI
					Faunus::MPI::ParticleTransmitter<Tpvec> pt;//!< Class for transmitting particles over MPI

					typedef std::function<double(Tspace&, Energy::Energybase<Tspace>&, const Tpvec&)> Tenergyfunc;
					Tenergyfunc usys; //!< Defaults to Energy::systemEnergy but can be replaced!

				public:
					ParallelTempering(
							Energy::Energybase<Tspace>&, Tspace&, Tmjson&, MPI::MPIController&);

					virtual ~ParallelTempering();

					void setCurrentEnergy(double); //!< Set energy before move (for increased speed)

					void setEnergyFunction( Tenergyfunc );
			};

		template<class Tspace>
			ParallelTempering<Tspace>::ParallelTempering(
					Energy::Energybase<Tspace> &e,
					Tspace &s,
					Tmjson &j,
					MPI::MPIController &mpi ) : base( e, s ) {

				this->title   = "Parallel Tempering";
				this->mpiPtr  = &mpi;
				partner=-1;
				this->useAlternativeReturnEnergy=true; //dont return dU from partner replica (=drift)
				this->runfraction = j.value("prob", 1.0);
				pt.recvExtra.resize(1);
				pt.sendExtra.resize(1);
				pt.setFormat( j.value("format", string("XYZQI") ) );

				setEnergyFunction(
						Energy::systemEnergy<Tspace,Energy::Energybase<Tspace>,Tpvec> );

				this->haveCurrentEnergy=false;

				if ( this->mpiPtr == nullptr )
					throw std::runtime_error(this->title + ": invalid MPIcontroller");
			}

		template<class Tspace>
			ParallelTempering<Tspace>::~ParallelTempering() {}

		template<class Tspace>
			void ParallelTempering<Tspace>::setEnergyFunction( Tenergyfunc f ) {
				usys = f;
			}

		template<class Tspace>
			void ParallelTempering<Tspace>::findPartner() {
				int dr=0;
				partner = mpiPtr->rank();
				if (mpiPtr->random()>0.5)
					dr++;
				else
					dr--;
				if (mpiPtr->rank() % 2 == 0)
					partner+=dr;
				else
					partner-=dr;
			}

		template<class Tspace>
			bool ParallelTempering<Tspace>::goodPartner() {
				assert(partner!=mpiPtr->rank() && "Selfpartner! This is not supposed to happen.");
				if (partner>=0)
					if ( partner<mpiPtr->nproc() )
						if ( partner!=mpiPtr->rank() )
							return true;
				return false;
			}

		template<class Tspace>
			string ParallelTempering<Tspace>::_info() {
				using namespace textio;
				std::ostringstream o;
				o << pad(SUB,w,"Process rank") << mpiPtr->rank() << endl
					<< pad(SUB,w,"Number of replicas") << mpiPtr->nproc() << endl
					<< pad(SUB,w,"Data size format") << short(pt.getFormat()) << endl
					<< indent(SUB) << "Acceptance:"
					<< endl;
				if (this->cnt>0) {
					o.precision(3);
					for (auto &m : accmap)
						o << indent(SUBSUB) << std::left << setw(12)
							<< m.first << setw(8) << m.second.cnt << m.second.avg()*100
							<< percent << endl;
				}
				return o.str();
			}

		template<class Tspace>
			void ParallelTempering<Tspace>::_trialMove() {
				findPartner();
				if (goodPartner()) {

					pt.sendExtra[VOLUME]=spc->geo.getVolume();  // copy current volume for sending

					pt.recv(*mpiPtr, partner, spc->trial); // receive particles
					pt.send(*mpiPtr, spc->p, partner);     // send everything
					pt.waitrecv();
					pt.waitsend();

					// update group trial mass-centers. Needed if energy calc. uses
					// cm_trial for cut-offs, for example
					for (auto g : spc->groupList())
						g->cm_trial = Geometry::massCenter(spc->geo, spc->trial, *g);

					// debug assertions
					assert(pt.recvExtra[VOLUME]>1e-6 && "Invalid partner volume received.");
					assert(spc->p.size() == spc->trial.size() && "Particle vectors messed up by MPI");

					// release assertions
					if (pt.recvExtra[VOLUME]<1e-6 || spc->p.size() != spc->trial.size())
						MPI_Abort(mpiPtr->comm, 1);
				}
			}

		/**
		 * If the system energy is already known it may be specified with this
		 * function to speed up the calculation. If not set, it will be calculated.
		 */
		template<class Tspace>
			void ParallelTempering<Tspace>::setCurrentEnergy(double uold) {
				currentEnergy=uold;
				haveCurrentEnergy=true;
			}

		template<class Tspace>
			double ParallelTempering<Tspace>::_energyChange() {
				this->alternateReturnEnergy=0;
				if ( !goodPartner() )
					return pc::infty;
				double uold, du_partner;

				if (haveCurrentEnergy)   // do we already know the energy?
					uold = currentEnergy;
				else
					uold = usys(*spc,*pot,spc->p);

				spc->geo.setVolume( pt.recvExtra[VOLUME] ); // set new volume
				pot->setSpace(*spc);

				double unew = usys(*spc,*pot,spc->trial);

				du_partner = exchangeEnergy(unew-uold); // Exchange dU with partner (MPI)

				haveCurrentEnergy=false;                // Make sure user call setCurrentEnergy() before next move
				this->alternateReturnEnergy=unew-uold;        // Avoid energy drift (no effect on sampling!)
				return (unew-uold)+du_partner;          // final Metropolis trial energy
			}

		/**
		 * This will exchange energies with replica partner
		 * @todo Use FloatTransmitter::swapf() instead.
		 *       Use C++11 initializer list for vectors, i.e. vector<floatp> v={mydu};
		 */
		template<class Tspace>
			double ParallelTempering<Tspace>::exchangeEnergy(double mydu) {
				vector<MPI::FloatTransmitter::floatp> duSelf(1), duPartner;
				duSelf[0]=mydu;
				duPartner = ft.swapf(*mpiPtr, duSelf, partner);
				return duPartner.at(0);               // return partner energy change
			}

		template<class Tspace>
			string ParallelTempering<Tspace>::id() {
				std::ostringstream o;
				if (mpiPtr->rank() < partner)
					o << mpiPtr->rank() << " <-> " << partner;
				else
					o << partner << " <-> " << mpiPtr->rank();
				return o.str();
			}

		template<class Tspace>
			void ParallelTempering<Tspace>::_acceptMove(){
				if ( goodPartner() ) {
					//temperPath << cnt << " " << partner << endl;
					accmap[ id() ] += 1;
					for (size_t i=0; i<spc->p.size(); i++)
						spc->p[i] = spc->trial[i];  // copy new configuration
					for (auto g : spc->groupList())
						g->cm = g->cm_trial;
				}
			}

		template<class Tspace>
			void ParallelTempering<Tspace>::_rejectMove() {
				if ( goodPartner() ) {
					spc->geo.setVolume( pt.sendExtra[VOLUME] ); // restore old volume
					pot->setSpace(*spc);
					accmap[ id() ] += 0;
					for (size_t i=0; i<spc->p.size(); i++)
						spc->trial[i] = spc->p[i];   // restore old configuration
					for (auto g : spc->groupList())
						g->cm_trial = g->cm;
				}
			}
#endif

		/**
		 * @brief Swap atom charges
		 *
		 * This move selects two particle index from a user-defined list and swaps
		 * their charges.
		 *
		 * @date Lund, 2013
		 */
		template<class Tspace>
			class SwapCharge : public Movebase<Tspace>
		{
			private:
				typedef Movebase<Tspace> base;
				typedef std::map<short, Average<double> > map_type;
				string _info() override;
			protected:
				void _acceptMove() override;
				void _rejectMove() override;
				double _energyChange() override;
				void _trialMove() override;
				using base::spc;
				using base::pot;
				map_type accmap; //!< Single particle acceptance map
				int ip, jp;

				//int iparticle;   //!< Select single particle to move (-1 if none, default)

			public:
				SwapCharge( Tmjson&, Energy::Energybase<Tspace> &, Tspace & );
				std::set<int> swappableParticles;  //!< Particle index that can be swapped
		};

		template<class Tspace>
			SwapCharge<Tspace>::SwapCharge( Tmjson &in, Energy::Energybase<Tspace> &e, Tspace &s ) : base(e, s)
		{
			base::title = "Swap head groups of different charges";
		}

		template<class Tspace>
			void SwapCharge<Tspace>::_trialMove()
			{
				assert(!swappableParticles.empty());
				auto vi = swappableParticles.begin();
				auto vj = swappableParticles.begin();
				//std::advance(vi, slump.rand() % swappableParticles.size());
				//std::advance(vj, slump.rand() % swappableParticles.size());
				//ip=*(vi);
				//jp=*(vj);
				ip = *(slump.element(swappableParticles.begin(), swappableParticles.end()));
				jp = *(slump.element(swappableParticles.begin(), swappableParticles.end()));
				if ( spc->trial[ip].charge != spc->trial[jp].charge )
				{
					std::swap(spc->trial[ip].charge, spc->trial[jp].charge);
				}
			}

		template<class Tspace>
			double SwapCharge<Tspace>::_energyChange()
			{
				return base::pot->i_total(spc->trial, jp) + base::pot->i_total(spc->trial, ip)
					- base::pot->i_total(spc->p, jp) - base::pot->i_total(spc->p, ip);
			}

		template<class Tspace>
			void SwapCharge<Tspace>::_acceptMove()
			{
				accmap[spc->p[ip].id] += 1;
				spc->p[ip].charge = spc->trial[ip].charge;
				spc->p[jp].charge = spc->trial[jp].charge;
			}

		template<class Tspace>
			void SwapCharge<Tspace>::_rejectMove()
			{
				accmap[spc->p[ip].id] += 0;
				spc->trial[ip].charge = spc->p[ip].charge;
				spc->trial[jp].charge = spc->p[jp].charge;
			}

		template<class Tspace>
			string SwapCharge<Tspace>::_info()
			{
				using namespace textio;
				std::ostringstream o;
				o << pad(SUB, base::w, "Average moves/particle")
					<< base::cnt / swappableParticles.size() << endl;
				if ( base::cnt > 0 )
				{
					char l = 12;
					o << endl
						<< indent(SUB) << "Individual particle movement:" << endl << endl
						<< indent(SUBSUB) << std::left << string(7, ' ')
						<< setw(l + 1) << "Acc. " + percent;
					for ( auto m : accmap )
					{
						auto id = m.first;
						o << indent(SUBSUB) << std::left << setw(7) << atom[id].name;
						o.precision(3);
						o << setw(l) << accmap[id].avg() * 100;
					}
				}
				return o.str();
			}

		/**
		 * @brief Flip-flop move of lipids in planar and cylindrical geometry
		 *
		 * Key                    | Description
		 * :--------------------- | :-----------------------------
		 * `flipflop_geometry`    | Geometry of the bilayer [planar(default) or cylindrical]
		 * `flipflop_runfraction` | Runfraction [default=1]
		 *
		 */
		template<class Tspace>
			class FlipFlop : public Movebase<Tspace>
		{
			private:
				typedef Movebase<Tspace> base;
			protected:
				using base::spc;
				using base::pot;
				using base::w;
				using base::cnt;
				void _trialMove() override;
				void _acceptMove() override;
				void _rejectMove() override;
				double _energyChange() override;
				string _info() override;
				typedef std::map<string, Average<double> > map_type;
				map_type accmap;   //!< Group particle acceptance map
				Group *igroup;
				Point *cntr;
				string geometry;
			public:
				FlipFlop( Tmjson &, Energy::Energybase<Tspace> &, Tspace & ); // if cylindrical geometry, string=cylinder
				void setGroup( Group & ); //!< Select Group to move
				void setCenter( Point & ); //!< Select Center of Mass of the bilayer
		};

		template<class Tspace>
			FlipFlop<Tspace>::FlipFlop( Tmjson &j, Energy::Energybase<Tspace> &e, Tspace &s ) : base(e, s)
		{
			base::title = "Group Flip-Flop Move";
			base::w = 30;
			igroup = nullptr;
			cntr = nullptr;
			geometry = j["geometry"] | string("planar");
			this->runfraction = j["prob"] | 1.0;
		}

		template<class Tspace>
			void FlipFlop<Tspace>::setGroup( Group &g )
			{
				assert(g.isMolecular());
				igroup = &g;
			}

		template<class Tspace>
			void FlipFlop<Tspace>::setCenter( Point &center )
			{
				cntr = &center;
			}

		template<class Tspace>
			void FlipFlop<Tspace>::_trialMove()
			{
				assert(igroup != nullptr);
				assert(cntr != nullptr);
				Point startpoint = spc->p[igroup->back()];
				Point endpoint = *cntr;
				startpoint.z() = cntr->z();
				if ( geometry.compare("cylindrical") == 0 )
				{ // MC move in case of cylindrical geometry
					startpoint = spc->p[igroup->back()];
					Point head = spc->p[igroup->front()];
					cntr->z() = head.z() = startpoint.z();
					Point dir = spc->geo.vdist(*cntr, startpoint)
						/ sqrt(spc->geo.sqdist(*cntr, startpoint)) * 1.1 * spc->p[igroup->back()].radius;
					if ( spc->geo.sqdist(*cntr, startpoint) > spc->geo.sqdist(*cntr, head))
						startpoint.translate(spc->geo, -dir); // set startpoint for rotation
					else
						startpoint.translate(spc->geo, dir);
					double x1 = cntr->x();
					double y1 = cntr->y();
					double x2 = startpoint.x();
					double y2 = startpoint.y();
					endpoint.x() = x2 + 1; // rot endpoint of axis ⊥ to line connecting cm of cylinder with 2nd TL
					endpoint.y() = -(x2 - x1) / (y2 - y1) + y2;
					endpoint.z() = startpoint.z();
				}
				double angle = pc::pi; // MC move in case of planar geometry
				Geometry::QuaternionRotate vrot;
				vrot.setAxis(spc->geo, startpoint, endpoint, angle); //rot around startpoint->endpoint vec
				for ( auto i : *igroup )
					spc->trial[i] = vrot(spc->trial[i]);
				igroup->cm_trial = vrot(igroup->cm_trial);
			}

		template<class Tspace>
			void FlipFlop<Tspace>::_acceptMove()
			{
				accmap[igroup->name] += 1;
				igroup->accept(*spc);
			}

		template<class Tspace>
			void FlipFlop<Tspace>::_rejectMove()
			{
				accmap[igroup->name] += 0;
				igroup->undo(*spc);
			}

		template<class Tspace>
			double FlipFlop<Tspace>::_energyChange()
			{

				for ( auto i : *igroup )
					if ( spc->geo.collision(spc->trial[i], spc->trial[i].radius, Geometry::Geometrybase::BOUNDARY))
						return pc::infty;

				double unew = pot->external(spc->trial) + pot->g_external(spc->trial, *igroup);
				if ( unew == pc::infty )
					return pc::infty;       // early rejection
				double uold = pot->external(spc->p) + pot->g_external(spc->p, *igroup);

				for ( auto g : spc->groupList())
				{
					if ( g != igroup )
					{
						unew += pot->g2g(spc->trial, *g, *igroup);
						if ( unew == pc::infty )
							return pc::infty;   // early rejection
						uold += pot->g2g(spc->p, *g, *igroup);
					}
				}
				return unew - uold;
			}

		template<class Tspace>
			string FlipFlop<Tspace>::_info()
			{
				using namespace textio;
				std::ostringstream o;
				if ( cnt > 0 )
				{
					char l = 12;
					o << indent(SUB) << "Move Statistics:" << endl
						<< indent(SUBSUB) << std::left << setw(20) << "Group name" //<< string(20,' ')
						<< setw(l + 1) << "Acc. " + percent << endl;
					for ( auto m : accmap )
					{
						string id = m.first;
						o << indent(SUBSUB) << std::left << setw(20) << id;
						o.precision(3);
						o << setw(l) << accmap[id].avg() * 100 << endl;
					}
				}
				return o.str();
			}

		/**
		 * @brief Grand Canonical Monte Carlo Move
		 *
		 * This is a general class for GCMC that can handle both
		 * atomic and molecular species at constant chemical potential.
		 *
		 * @todo Currently tested only with rigid, molecular species. Move
		 *       external energy calculation into Hamiltonian. Move particle
		 *       density analysis to Faunus::Analysis.
		 *
		 * @date Brno/Lund 2014-2015
		 * @author Lukas Sukenik and Mikael Lund
		 */
		template<class Tspace, class base=Movebase<Tspace> >
			class GreenGC : public base
		{
			private:

				typedef typename Tspace::ParticleVector Tpvec;
				using base::spc;
				using base::pot;
				using base::w;

				vector<Group *> molDel;               // groups to delete
				vector<int> atomDel;                 // atom index to delete
				MoleculeCombinationMap<Tpvec> comb;  // map of combinations to insert
				std::map<int, int> molcnt, atomcnt;   // id's and number of inserted/deleted mols and atoms
				std::multimap<int, Tpvec> pmap;      // coordinates of mols and atoms to be inserted
				unsigned int Ndeleted, Ninserted;    // Number of accepted deletions and insertions
				bool insertBool;                     // current status - either insert or delete
				typename MoleculeCombinationMap<Tpvec>::iterator it; // current combination

				/** @brief Perform an insertion or deletion trial move */
				void _trialMove() override
				{

					// pick random combination and count mols and atoms
					base::alternateReturnEnergy = 0;
					molcnt.clear();
					atomcnt.clear();
					it = comb.random();                 // random combination
					for ( auto id : it->molComb )
					{     // loop over molecules in combination
						if ( spc->molecule[id].isAtomic())
							for ( auto i : spc->molecule[id].atoms )
								atomcnt[i]++;                 // count number of atoms per type
						else
							molcnt[id]++;                   // count number of molecules per type
					}

					insertBool = slump.range(0, 1) == 1;

					// try delete move
					if ( !insertBool )
					{
						molDel.clear();
						atomDel.clear();

						// find atom index and group pointers
						bool empty = false;           // true if too few atoms/mols are present
						for ( auto &a : atomcnt )     // (first=type, second=count)
							if ( !spc->atomTrack.find(a.first, a.second, atomDel))
								empty = true;
						for ( auto &m : molcnt )
							if ( !spc->molecule[m.first].isAtomic())
								if ( !spc->molTrack.find(m.first, m.second, molDel))
									empty = true;
						if ( empty )
						{        // nothing left to delete
							molDel.clear();
							atomDel.clear();
							pmap.clear();
						}
						else
							assert(!molDel.empty() || !atomDel.empty());
					}

					// try insert move (nothing is actually inserted - just a proposed configuration)
					if ( insertBool )
					{
						pmap.clear();
						for ( auto molid : it->molComb ) // loop over molecules in combination
							pmap.insert(
									{molid, spc->molecule[molid].getRandomConformation(base::spc->geo, base::spc->p)});
						assert(!pmap.empty());
					}
				}

				// contribution from external chemical potential
				// TODO: move into Hamiltonian
				double externalEnergy()
				{
					double u = 0;
					double V = spc->geo.getVolume();
					int bit = (insertBool) ? 1 : 0;
					double sign = (insertBool) ? 1 : -1;

					for ( auto i : molcnt )                  // loop over molecule types
						if ( !spc->molecule[i.first].isAtomic())
							for ( int n = 0; n < i.second; n++ )       // loop over n number of molecules
								u += log((spc->molTrack.size(i.first) + bit) / V) - spc->molecule[i.first].chemPot;

					for ( auto i : atomcnt )                 // loop over atom types
						for ( int n = 0; n < i.second; n++ )         // loop over n number of atoms
							u += log((spc->atomTrack.size(i.first) + bit) / V) - atom[i.first].chemPot;

					return sign * u;
				}

				double _energyChange() override
				{

					double u = 0;         // change in potential energy (kT)
					double uinternal = 0; // change in internal, molecular energy (kT)

					// energy if insertion move
					if ( insertBool )
					{
						for ( auto &p : pmap )
						{                         // loop over molecules
							Group g(0, p.second.size() - 1);               // (first=id, second=pvec)
							g.molId = p.first;
							g.setMolSize(p.second.size());

							u += pot->g_external(p.second, g);           // ...atoms/mols with external pot

							if ( spc->molecule[g.molId].isAtomic())
							{
								u += pot->g_internal(p.second, g);         // ...between inserted atoms
								for ( auto &pi : p.second )                  // ...atoms with all particles
									u += pot->all2p(spc->p, pi);
							}
							else
							{
								for ( auto g2 : spc->groupList())           // ...molecules with all groups
									u += pot->g1g2(p.second, g, spc->p, *g2);
								uinternal += pot->g_internal(p.second, g); // ...internal mol energy (dummy)
							}
						}

						for ( auto i = pmap.begin(); i != pmap.end(); ++i )  //...between inserted molecules
							for ( auto j = i; ++j != pmap.end(); )
							{
								Group gi(0, i->second.size() - 1);
								Group gj(0, i->second.size() - 1);
								gi.molId = i->first;
								gj.molId = j->first;
								u += pot->g1g2(i->second, gi, j->second, gj);
							}

						assert(!pmap.empty());
						base::alternateReturnEnergy = u + uinternal;
						return u + externalEnergy();
					}

					// energy if deletion move
					else
					{
						if ( !molDel.empty() || !atomDel.empty())
						{
							for ( auto i : molDel )
							{                     // loop over molecules/atoms
								u += pot->g_external(spc->p, *i);         // molecule w. external pot.

								if ( !spc->molecule[i->molId].isAtomic())
								{
									for ( auto j : spc->groupList())         // molecule w. all groups
										if ( find(molDel.begin(), molDel.end(), j) == molDel.end()) // slow!
											u += pot->g2g(spc->p, *i, *j);
									uinternal += pot->g_internal(spc->p, *i);// internal mol energy (dummy)
								}
							}

							// energy between deleted molecules
							for ( auto i = molDel.begin(); i != molDel.end(); ++i )
								for ( auto j = i; ++j != molDel.end(); )
									u += pot->g2g(spc->p, **i, **j);

							for ( auto i : atomDel )                        // atoms w. all particles
								u += pot->i_total(spc->p, i);

							for ( int i = 0; i < (int) atomDel.size() - 1; i++ )   // subtract double counted
								for ( int j = i + 1; j < (int) atomDel.size(); j++ ) // internal energy (atoms)
									u -= pot->i2i(spc->p, i, j);

							base::alternateReturnEnergy = -u - uinternal;
							return -u + externalEnergy(); // ...add activity terms
						}
					}

					// if we reach here, we're out of particles -> reject

					assert(!insertBool);
					assert(fabs(u) < 1e-10);
					base::alternateReturnEnergy = 0;
					return pc::infty;
				}

				void _acceptMove() override
				{

					// accept a deletion move
					if ( !insertBool )
					{
						Ndeleted++;
						for ( auto m : molDel ) // loop over Group pointers
							base::spc->eraseGroup(spc->findIndex(m));

						for ( auto i : atomDel )
						{// loop over particle index
							assert(1 == 2 && "Under construction");
							base::spc->erase(i);
						}
					}

					// accept an insertion move
					if ( insertBool )
					{
						Ninserted++;
						for ( auto &p : pmap )
						{ // loop over sets of new coordinates
							auto molid = p.first;
							if ( spc->molecule[molid].isAtomic())
							{
								assert(1 == 2 && "Under construction");
								spc->insert(molid, p.second);
							}
							else
							{
								assert(!p.second.empty());
								spc->insert(molid, p.second); // auto gen. group
								assert(spc->molTrack.size(molid) > 0);
							}
						}
					}
					spc->molTrack.updateAvg();   // update average number of molecules
					spc->atomTrack.updateAvg();  // ...and atoms
					pot->setSpace(*spc);
				}

				void _rejectMove() override
				{
					spc->molTrack.updateAvg();   // update average number of molecules
					spc->atomTrack.updateAvg();  // ...and atoms
				}

				string _info() override
				{
					using namespace textio;
					std::ostringstream o;

					o << pad(SUB, base::w, "Accepted insertions") << Ninserted << "\n"
						<< pad(SUB, base::w, "Accepted deletions") << Ndeleted << "\n"
						<< pad(SUB, base::w, "Flux (Nins/Ndel)") << Ninserted / double(Ndeleted) << "\n"
						<< "\n";

					double V = spc->geo.getVolume();
					o << std::left
						<< setw(w + 5) << "  Molecule/Atom"
						<< setw(w) << "a (mol/l)"
						<< setw(w) << "c (mol/l)"
						<< setw(w) << textio::gamma + "=a/c" << "\n"
						<< "  " << string(4 * w, '-') << "\n";

					for ( auto &m : spc->molecule )
					{
						if ( m.activity > 1e-10 )
							if ( spc->molTrack.getAvg(m.id).cnt > 0 )
								o << setw(w + 5) << ("  " + m.name) << setw(w) << m.activity
									<< setw(w) << spc->molTrack.getAvg(m.id) / V / 1.0_molar
									<< setw(w) << m.activity / (spc->molTrack.getAvg(m.id) / V / 1.0_molar) << "\n";
					}

					o << "\n";

					for ( auto &m : atom )
					{
						if ( m.activity > 1e-6 )
							if ( spc->atomTrack.getAvg(m.id).cnt > 0 )
								o << setw(w + 5) << ("  " + m.name) << setw(w) << m.activity
									<< setw(w) << spc->atomTrack.getAvg(m.id) / V / 1.0_molar
									<< setw(w) << m.activity / (spc->atomTrack.getAvg(m.id) / V / 1.0_molar) << "\n";
					}

					return o.str() + spc->molecule.info() + comb.info();
				}

				void _test( UnitTest &t ) override
				{
					string jsondir = textio::trim(base::title);
					double V = spc->geo.getVolume();
					t(jsondir + "_flux", Ninserted / double(Ndeleted));
					for ( auto &m : spc->molecule )
						if ( m.activity > 1e-6 )
							if ( spc->molTrack.getAvg(m.id).cnt > 0 )
								if ( !m.name.empty())
									t(jsondir + "_mol_" + m.name + "_gamma",
											m.activity / (spc->molTrack.getAvg(m.id) / V / 1.0_molar));
					for ( auto &m : atom )
						if ( m.activity > 1e-6 )
							if ( !m.name.empty())
								if ( spc->atomTrack.getAvg(m.id).cnt > 0 )
									t(jsondir + "_atom_" + m.name + "_gamma",
											m.activity / (spc->atomTrack.getAvg(m.id) / V / 1.0_molar));
				}

				void init()
				{ // call this upon construction
					Ninserted = 0;
					Ndeleted = 0;
					base::title = "Grand Canonical";
					base::useAlternativeReturnEnergy = true;
				}

			public:

				/** @brief Constructor -- load combinations, initialize trackers */
				GreenGC(
						Energy::Energybase<Tspace> &e, Tspace &s, Tmjson &j ) : base(e, s), comb(s.molecule)
				{
					init();
					base::runfraction = j.value("prob", 1.0);
					comb.include(j); // load combinations
				}
		};

		/**
		 * @brief Move for swapping species types - i.e. implicit titration
		 *
		 * Upon construction this class will add an instance of
		 * `Energy::EquilibriumEnergy` to the Hamiltonian. For details
		 * about the titration procedure see `Energy::EquilibriumController`.
		 *
		 *
		 * Input:
		 *
		 *  Keyword        |  Description
		 *  :------------- |  :---------------------------------
		 * `prob`          |  probability of running (default: 1)
		 * `savecharge`    |  save average charge upon destruction (default: false)
		 * `processes`     |  List of equilibrium processes, see `Energy::EquilibriumController`
		 */
		template<class Tspace>
			class SwapMove : public Movebase<Tspace>
		{
			private:
				typedef Movebase<Tspace> base;
				std::map<int, Average<double> > accmap; //!< Site acceptance map
				string _info() override;
				void _trialMove() override;
				void _acceptMove() override;
				void _rejectMove() override;

				bool saveChargeBool;

				std::map<int, std::map<int, Average<double> >> molCharge;

				void updateMolCharge( int pindex )
				{
					auto g = spc->findGroup(pindex);
					molCharge[g->molId][pindex - g->front()] += spc->p[pindex].charge;
				}

			protected:
				using base::spc;
				using base::pot;

				double _energyChange() override;
				int ipart;                              //!< Particle to be swapped
				Energy::EquilibriumEnergy<Tspace> *eqpot;

			public:
				template<class Tenergy>
					SwapMove( Tenergy &, Tspace &, Tmjson & ); //!< Constructor

				~SwapMove()
				{
					if ( saveChargeBool )
						if ( this->runfraction > 1e-3 )
						{
							applyCharges(spc->p);
							FormatAAM::save("avgcharge.aam", spc->p);
							FormatPQR::save("avgcharge.pqr", spc->p);
							spc->p = spc->trial;
						}
				}

				template<class Tpvec>
					int findSites( const Tpvec & ); //!< Search for titratable sites (old ones are discarded)

				double move( int n = 1 ) override
				{
					double du = 0;
					if ( this->run())
					{
						eqpot->findSites(this->spc->p);
						size_t i = eqpot->eq.sites.size();
						while ( i-- > 0 )
							du += base::move();
						eqpot->eq.sampleCharge(spc->p);
					}
					return du;
				}

				/** @brief Copy average charges into particle vector */
				template<class Tpvec>
					void applyCharges( Tpvec &p )
					{
						for ( auto &g : spc->groupList()) // loop over all groups
							for ( auto &i : molCharge[g->molId] ) // loop over particles
								p[g->front() + i.first].charge = i.second;
					}
		};

		template<class Tspace>
			template<class Tenergy> SwapMove<Tspace>::SwapMove(
					Tenergy &e, Tspace &spc, Tmjson &j ) : base(e, spc)
			{

				base::title = "Site Titration - Swap Move";
				base::runfraction = j.value("prob", 1.0);
				base::w = 30;
				ipart = -1;

				saveChargeBool = j.value("savecharge", false);

				auto t = e.tuple();
				auto ptr = TupleFindType::get<Energy::EquilibriumEnergy<Tspace> *>(t);
				if ( ptr != nullptr )
					eqpot = *ptr;
				else
					throw std::runtime_error("`EquilibriumEnergy` required in Hamiltonian.");

				eqpot->eq = Energy::EquilibriumController(j);

				if ( base::runfraction > 1e-4 )
					findSites(spc.p);

				/* Sync particle charges with `AtomMap` */
				for ( auto i : eqpot->eq.sites )
					spc.trial[i].charge = spc.p[i].charge = atom[spc.p[i].id].charge;
			}

		/**
		 * @brief Search for titratable sites and store internally
		 *
		 * Use this to re-scan for titratable sites. Called by default
		 * in the constructor
		 */
		template<class Tspace>
			template<class Tpvec>
			int SwapMove<Tspace>::findSites( const Tpvec &p )
			{
				accmap.clear();
				return eqpot->findSites(p);
			}

		template<class Tspace>
			void SwapMove<Tspace>::_trialMove()
			{
				if ( !eqpot->eq.sites.empty())
				{
					int i = slump.range(0, eqpot->eq.sites.size() - 1); // pick random site
					ipart = eqpot->eq.sites.at(i);                      // and corresponding particle
					int k;
					do
					{
						k = slump.range(0, eqpot->eq.process.size() - 1);// pick random process..
					}
					while ( !eqpot->eq.process[k].one_of_us(this->spc->p[ipart].id)); //that match particle j

					eqpot->eq.process[k].swap(this->spc->trial[ipart]); // change state and get intrinsic energy change
				}
			}

		template<class Tspace>
			double SwapMove<Tspace>::_energyChange()
			{
				assert(spc->geo.collision(spc->p[ipart], spc->p[ipart].radius) == false
						&& "Accepted particle collides with container");

				if ( spc->geo.collision(spc->trial[ipart], spc->trial[ipart].radius))  // trial<->container collision?
					return pc::infty;
				double uold = pot->external(spc->p) + pot->i_total(spc->p, ipart);
				double unew = pot->external(spc->trial) + pot->i_total(spc->trial, ipart);
#ifdef ENABLE_MPI
				if ( base::mpiPtr != nullptr ) {
					double sum=0;
					auto r = Faunus::MPI::splitEven(*base::mpiPtr, (int)spc->p.size());
					for (int i=r.first; i<=r.second; ++i)
						if (i!=ipart)
							sum+=pot->i2i(spc->trial,i,ipart) - pot->i2i(spc->p,i,ipart);

					sum = Faunus::MPI::reduceDouble(*base::mpiPtr, sum);

					return sum + pot->i_external(spc->trial, ipart) - pot->i_external(spc->p, ipart)
						+ pot->i_internal(spc->trial, ipart) - pot->i_internal(spc->p, ipart);
				}
#endif

				return unew - uold;
			}

		template<class Tspace>
			void SwapMove<Tspace>::_acceptMove()
			{
				accmap[ipart] += 1;
				spc->p[ipart] = spc->trial[ipart];
				updateMolCharge(ipart);
				// atom type changed -- update atom tracker
				spc->atomTrack.erase(ipart);
				spc->atomTrack.insert(spc->p[ipart].id, ipart);
			}

		template<class Tspace>
			void SwapMove<Tspace>::_rejectMove()
			{
				accmap[ipart] += 0;
				spc->trial[ipart] = spc->p[ipart];
				updateMolCharge(ipart);
			}

		template<class Tspace>
			string SwapMove<Tspace>::_info()
			{
				using namespace textio;
				std::ostringstream o;
				for ( auto &m : molCharge )
				{
					int molid = m.first;
					o << "\n" << indent(SUB) << "Molecule: " << spc->molList()[molid].name << "\n\n"
						<< std::left << "    " << setw(8) << "index" << setw(12) << "name"
						<< setw(12) << "Z" << "\n";
					for ( auto &i : m.second )
						o << "    " << setw(8) << i.first
							<< setw(12) << atom[spc->molList()[molid].atoms[i.first]].name
							<< setw(12) << i.second << "\n";
				}
				return o.str();
			}

		/**
		 * @brief As SwapMove but Minimizes Short Ranged interactions
		 *        within a molecule upon swapping
		 *
		 * Before calculating dU of an attempted swap move, radii on
		 * particles within the SAME group are set to minus radius of
		 * the swapped particle and hydrophobicity is set to false.
		 * This to minimize large interactions in molecules with overlapping
		 * particles - i.e LJ will be zero. It can also be used to avoid
		 * internal hydrophobic interactions in rigid groups upon swapping
		 * between hydrophobic and non-hydrophobic species.
		 */
		template<class Tspace>
			class SwapMoveMSR : public SwapMove<Tspace>
		{
			private:
				using SwapMove<Tspace>::spc;
				using SwapMove<Tspace>::pot;
				std::map<int, double> radiusbak;    // backup for radii
				std::map<int, bool> hydrophobicbak; // backup for hydrophobic state

				void modify()
				{
					radiusbak.clear();
					hydrophobicbak.clear();
					for ( auto g : spc->groupList())   // loop over all groups
						if ( g->find(this->ipart))
						{  //   is ipart part of a group?
							for ( auto i : *g )    //     if so, loop over that group
								if ( i != this->ipart )
								{    //       and ignore ipart
									assert(abs(spc->p[i].radius - spc->trial[i].radius) < 1e-9);
									assert(spc->p[i].hydrophobic == spc->trial[i].hydrophobic);

									//radiusbak[i]         = spc->p[i].radius;
									//spc->p[i].radius     = -spc->p[ipart].radius;
									//spc->trial[i].radius = -spc->p[ipart].radius;

									hydrophobicbak[i] = spc->p[i].hydrophobic;
									spc->p[i].hydrophobic = false;
									spc->trial[i].hydrophobic = false;
								}
							return; // a particle can be part of a single group, only
						}
				}

				void restore()
				{
					for ( auto &m : radiusbak )
					{
						spc->p[m.first].radius = m.second;
						spc->trial[m.first].radius = m.second;
					}
					for ( auto &m : hydrophobicbak )
					{
						spc->p[m.first].hydrophobic = m.second;
						spc->trial[m.first].hydrophobic = m.second;
					}
				}

				double _energyChange()
				{
					double du_orig = SwapMove<Tspace>::_energyChange();
					modify();
					double du = SwapMove<Tspace>::_energyChange();
					restore();
					this->alternateReturnEnergy = du_orig;
					return du;
				}

			public:
				SwapMoveMSR(
						Tmjson &in, Energy::Energybase<Tspace> &ham, Tspace &spc ) : SwapMove<Tspace>(in, ham, spc)
				{
					this->title += " (min. shortrange)";
					this->useAlternativeReturnEnergy = true;
				}
		};

		/**
		 * @brief Multiple moves controlled via JSON input
		 *
		 * This is a move class that randomly picks between a number of
		 * moves as defined in a JSON file in the section `moves`.
		 * The available moves are shown
		 * in the table below; each can occur only once and are picked
		 * with uniform weight.
		 *
		 * Keyword           | Class                      | Description
		 * :---------------- | :------------------------  | :----------------
		 * `atomtranslate`   | `Move::AtomicTranslation`  | Translate atoms
		 * `atomrotate`      | `Move::AtomicRotation`     | Rotate atoms
		 * `atomgc`          | `Move::GrandCanonicalSalt` | GC salt move (muVT ensemble)
		 * `atomtranslate2D` | `Move::AtomicTranslation2D`| Translate atoms on a 2D hypersphere
		 * `conformationswap`| `Move::ConformationSwap`   | Swap between molecular conformations
		 * `crankshaft`      | `Move::CrankShaft`         | Crank shaft polymer move
		 * `ctransnr`        | `Move::ClusterTranslateNR` | Rejection free cluster translate
		 * `gc`              | `Move::GreenGC`            | Grand canonical move (muVT ensemble)
		 * `isobaric`        | `Move::Isobaric`           | Volume move (NPT ensemple)
		 * `moltransrot`     | `Move::TranslateRotate`    | Translate/rotate molecules
		 * `pivot`           | `Move::Pivot`              | Pivot polymer move
		 * `reptate`         | `Move::Reptation`          | Reptation polymer move
		 * `temper`          | `Move::ParallelTempering`  | Parallel tempering (requires MPI)
		 * `titrate`         | `Move::SwapMove`           | Particle swap move
		 * `xtcmove`         | `Move::TrajectoryMove`     | Propagate via a filed trajectory
		 * `random`          | `RandomTwister<>`          | Input for random number generator
		 * `_jsonfile`       |  ouput json file name      | Default: `move_out.json`
		 *
		 * Average system energy and drift thereof are automatically tracked and
		 * reported.
		 *
		 * In addition to a global random number generator, the move classes
		 * share a unique (static) random number generator that dictates the
		 * Markov Chains. By default the state of the latter is copied from the
		 * former upon construction of `Propagator`.
		 * To instead attempt a _non-deterministric seed_, add
		 *
		 *     "random" : { "hardware":true }
		 *
		 * Upon destruction of the class, a JSON file is written to disk with
		 * details about each move. The output filename can be controlled by i.e.,
		 *
		 *     "_jsonfile" : "move_out.json"
		 *
		 * If the string is empty, no file will be written.
		 * See @ref inputoutput for more information about pretty printing
		 * JSON output.
		 */
		template<typename Tspace, bool polarise = false, typename base=Movebase<Tspace>>
			class Propagator : public base
		{
			private:
				typedef std::unique_ptr<base> basePtr;
				std::vector<basePtr> mPtr;
				Tspace *spc;
				string jsonfile; // output json file name

				double uinit; // initial energy evaluated just *before* first move
				double dusum; // sum of all energy *changes* by moves
				Average<double> uavg; // average system energy
				std::function<double()> ufunction; // function to calculate system energy

				string _info() override
				{
					using namespace textio;

					double ucurr = ufunction(); // current system energy

					std::ostringstream o;
					if ( uavg.cnt > 0 )
					{
						o << pad(SUB, base::w, "Average energy") << uavg.avg() << "\n"
							<< pad(SUB, base::w, "Initial energy") << uinit << kT << "\n"
							<< pad(SUB, base::w, "Current energy") << ucurr << kT << "\n"
							<< pad(SUB, base::w, "Changed") << dusum << kT << "\n"
							<< pad(SUB, base::w, "Absolute drift") << ucurr - (uinit + dusum) << kT << "\n"
							<< pad(SUB, base::w, "Relative drift") << (ucurr - (uinit + dusum)) / uinit * 100 << percent << "\n";

						for ( auto &i : mPtr )
							o << i->info();
					}
					return o.str();
				}

				void _acceptMove() override { assert(1 == 2); }

				void _rejectMove() override { assert(1 == 2); }

				void _trialMove() override { assert(1 == 2); }

				double _energyChange() override
				{
					assert(1 == 2);
					return 0;
				}

				template<typename Tmove>
					basePtr toPtr( Tmove m )
					{
						typedef typename std::conditional<polarise, PolarizeMove<Tmove>, Tmove>::type T;
						return basePtr(new T(m)); // convert to std::make_unique<>() in C++14
					}

			public:
				template<typename Tenergy>
#ifdef ENABLE_MPI
					Propagator( Tmjson &in, Tenergy &e, Tspace &s, MPI::MPIController *mpi=nullptr) : base(e, s), dusum(0)
#else
															  Propagator( Tmjson &in, Tenergy &e, Tspace &s) : base(e, s), dusum(0)
#endif
			{
				this->title = "P R O P A G A T O R S";

				jsonfile = "move_out.json";

				auto m = in.at("moves");
				for ( auto i = m.begin(); i != m.end(); ++i )
				{
					auto &val = i.value();

					try {

						if ( i.key() == "_jsonfile" )
							if (val.is_string())
								jsonfile = val;

						base::_slump().eng = slump.eng; // seed from global slump() instance

						if ( i.key() == "random" )
							if (val.is_object()) {
								cout << "Seeding move random number generator." << endl;
								base::_slump() = RandomTwister<>(val);
							}

						if ( i.key() == "atomtranslate" )
							mPtr.push_back(toPtr(AtomicTranslation<Tspace>(e, s, val)));
						if ( i.key() == "atomrotate" )
							mPtr.push_back(toPtr(AtomicRotation<Tspace>(e, s, val)));
						if ( i.key() == "atomgc" )
							mPtr.push_back(toPtr(GrandCanonicalSalt<Tspace>(e, s, val)));
						if (i.key()=="atomictranslation2D")
							mPtr.push_back( toPtr( AtomicTranslation2D<Tspace>(e,s, val)));
						if ( i.key() == "gctit" )
							mPtr.push_back(toPtr(GrandCanonicalTitration<Tspace>(e, s, val)));
						if ( i.key() == "moltransrot" )
							mPtr.push_back(toPtr(TranslateRotate<Tspace>(e, s, val)));
						if ( i.key() == "conformationswap" )
							mPtr.push_back(toPtr(ConformationSwap<Tspace>(e, s, val)));
						if ( i.key() == "moltransrot2body" )
							mPtr.push_back(toPtr(TranslateRotateTwobody<Tspace>(e, s, val)));
						if ( i.key() == "moltransrotcluster" )
							mPtr.push_back(toPtr(TranslateRotateCluster<Tspace>(e, s, val)));
						if ( i.key() == "ClusterMove" )
							mPtr.push_back(toPtr(ClusterMove<Tspace>(e, s, val)));
						if ( i.key() == "isobaric" )
							mPtr.push_back(toPtr(Isobaric<Tspace>(e, s, val)));
						if ( i.key() == "isochoric" )
							mPtr.push_back(toPtr(Isochoric<Tspace>(e, s, val)));
						if ( i.key() == "gc" )
							mPtr.push_back(toPtr(GreenGC<Tspace>(e, s, val)));
						if ( i.key() == "titrate" )
							mPtr.push_back(toPtr(SwapMove<Tspace>(e, s, val)));
						if ( i.key() == "crankshaft" )
							mPtr.push_back(toPtr(CrankShaft<Tspace>(e, s, val)));
						if ( i.key() == "pivot" )
							mPtr.push_back(toPtr(Pivot<Tspace>(e, s, val)));
						if ( i.key() == "reptate" )
							mPtr.push_back(toPtr(Reptation<Tspace>(e, s, val)));
						if ( i.key() == "ctransnr" )
							mPtr.push_back(toPtr(ClusterTranslateNR<Tspace>(e, s, val)));
						if ( i.key() == "xtcmove" )
							mPtr.push_back(toPtr(TrajectoryMove<Tspace>(e, s, val)));
#ifdef ENABLE_MPI
						if ( i.key() == "temper" )
							if (mpi!=nullptr)
								mPtr.push_back(toPtr(ParallelTempering<Tspace>(e, s, val, *mpi)));
#endif
					}
					catch (std::exception &e) {
						std::cerr << "Moves initialization error: " << i.key() << endl;
						throw;
					}
				}
				if ( mPtr.empty())
					throw std::runtime_error("No moves defined - check JSON file.");

				// Bind function to calculate initial system energy
				using std::ref;
				ufunction = std::bind(
						Energy::systemEnergy<Tspace, Tenergy, typename Tspace::ParticleVector>,
						ref(s), ref(e), ref(s.p));
			}

				~Propagator()
				{
					if (!jsonfile.empty())
						if (this->cnt>0) {
							std::ofstream f(textio::prefix + jsonfile);
							if (f)
								f << std::setw(4) << json() << endl;
						}
				}

				/** @brief Append move to list */
				basePtr append( base &m )
				{
					mPtr.push_back(basePtr(m));
					return mPtr.back();
				}

				double move( int n = 1 ) override
				{
					this->cnt++;
					double du = 0;
					if ( mPtr.empty())
						return du;

					if ( uavg.cnt == 0 )
						uinit = ufunction(); // calculate initial energy, prior to any moves

					du = (*base::_slump().element(mPtr.begin(), mPtr.end()))->move();
					dusum += du;
					uavg += uinit + dusum; // sample average system energy
					return du;  // return energy change
				}

				/** @brief Generate JSON object w. move information */
				Tmjson json()
				{
					Tmjson js;
					auto &j = js["moves"];
					for ( auto &i : mPtr )
						j = merge(j, i->json());
					j["random"] = base::_slump().json();
					return js;
				}

				void test( UnitTest &t )
				{
					for ( auto &i : mPtr )
						i->test(t);

					if (uavg.cnt>0) {
						double ucurr = ufunction();
						double drift = ucurr - (uinit + dusum);
						t("energyAverage", uavg.avg());
						t("relativeEnergyDrift", std::fabs(drift / ucurr), 1000.0);
					}
				}

#ifdef ENABLE_MPI
				void setMPI( Faunus::MPI::MPIController* mpi )
				{
					base::mpiPtr = mpi;
					for ( auto &i : mPtr )
						i->mpiPtr = mpi;
				}
#endif

		};

		/** @brief Atomic translation with dipolar polarizability */
		//typedef PolarizeMove<AtomicTranslation> AtomicTranslationPol;

		/** @brief Atomic rotation with dipolar polarizability */
		//typedef PolarizeMove<AtomicRotation> AtomicRotationPol;

	}//namespace
}//namespace
#endif
