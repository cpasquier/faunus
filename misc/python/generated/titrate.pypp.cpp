// This file has been generated by Py++.

#include "boost/python.hpp"
#include "generated/generated_header.h"
#include "titrate.pypp.hpp"

namespace bp = boost::python;

void register_titrate_class(){

    { //::Faunus::titrate
        typedef bp::class_< Faunus::titrate > titrate_exposer_t;
        titrate_exposer_t titrate_exposer = titrate_exposer_t( "titrate", bp::init< double >(( bp::arg("arg0") )) );
        bp::scope titrate_scope( titrate_exposer );
        bp::implicitly_convertible< double, Faunus::titrate >();
        titrate_exposer.def( bp::init< std::vector< Faunus::particle > &, Faunus::group &, double >(( bp::arg("arg0"), bp::arg("arg1"), bp::arg("arg2") )) );
        { //::Faunus::titrate::applycharges
        
            typedef double ( ::Faunus::titrate::*applycharges_function_type )( ::std::vector< Faunus::particle > & ) ;
            
            titrate_exposer.def( 
                "applycharges"
                , applycharges_function_type( &::Faunus::titrate::applycharges )
                , ( bp::arg("arg0") ) );
        
        }
        { //::Faunus::titrate::avgcharge
        
            typedef double ( ::Faunus::titrate::*avgcharge_function_type )( ::std::vector< Faunus::particle > &,unsigned int ) ;
            
            titrate_exposer.def( 
                "avgcharge"
                , avgcharge_function_type( &::Faunus::titrate::avgcharge )
                , ( bp::arg("arg0"), bp::arg("arg1") ) );
        
        }
        { //::Faunus::titrate::infos
        
            typedef void ( ::Faunus::titrate::*infos_function_type )(  ) ;
            
            titrate_exposer.def( 
                "infos"
                , infos_function_type( &::Faunus::titrate::infos ) );
        
        }
        { //::Faunus::titrate::init
        
            typedef void ( ::Faunus::titrate::*init_function_type )( ::std::vector< Faunus::particle > &,::Faunus::group & ) ;
            
            titrate_exposer.def( 
                "init"
                , init_function_type( &::Faunus::titrate::init )
                , ( bp::arg("arg0"), bp::arg("arg1") ) );
        
        }
        { //::Faunus::titrate::samplesites
        
            typedef void ( ::Faunus::titrate::*samplesites_function_type )( ::std::vector< Faunus::particle > & ) ;
            
            titrate_exposer.def( 
                "samplesites"
                , samplesites_function_type( &::Faunus::titrate::samplesites )
                , ( bp::arg("arg0") ) );
        
        }
        { //::Faunus::titrate::showsites
        
            typedef void ( ::Faunus::titrate::*showsites_function_type )( ::std::vector< Faunus::particle > & ) ;
            
            titrate_exposer.def( 
                "showsites"
                , showsites_function_type( &::Faunus::titrate::showsites )
                , ( bp::arg("arg0") ) );
        
        }
        { //::Faunus::titrate::sumsites
        
            typedef double ( ::Faunus::titrate::*sumsites_function_type )(  ) ;
            
            titrate_exposer.def( 
                "sumsites"
                , sumsites_function_type( &::Faunus::titrate::sumsites ) );
        
        }
        titrate_exposer.def_readwrite( "ph", &Faunus::titrate::ph );
    }

}