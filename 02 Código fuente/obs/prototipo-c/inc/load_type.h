#ifndef _LOAD__TYPE_H_
#define _LOAD__TYPE_H_

#include <stdint.h>

#include "apes_types.h"

typedef struct {
    string_t name;
    string_t tag;
} load_type_t;

typedef enum {
	LOAD_TYPE_EMERGENCY_LIGHTING,
	LOAD_TYPE_LIGHTING,
	LOAD_TYPE_CAPACITOR_BANK,
	LOAD_TYPE_CCTV,
	LOAD_TYPE_DATA_CENTER,
	LOAD_TYPE_PHOTOVOLTAIC,
	LOAD_TYPE_POWER_OUTLET,
	LOAD_TYPE_GENERAL,
	LOAD_TYPE_BACKUP_GENERATOR,
	LOAD_TYPE_HVAC,
	LOAD_TYPE_INSTRUMENTATION_CONTROL,
	LOAD_TYPE_VARIABLE_FREQUENCY_DRIVE,
	LOAD_TYPE_MOTOR,
	LOAD_TYPE_OFFICE_WORKSTATIONS,
	LOAD_TYPE_EV_CHARGING,
	LOAD_TYPE_RESERVE,
	LOAD_TYPE_SPLIT,
	LOAD_TYPE_AUXILIARY_SERVICES
} load_type_id_t;

static const load_type_t load_types[] = {
	[LOAD_TYPE_EMERGENCY_LIGHTING]			= { "Alumbrado de emergencia",	        "AE"	},
	[LOAD_TYPE_LIGHTING]					= { "Alumbrado",        		        "AL"	},
	[LOAD_TYPE_CAPACITOR_BANK]				= { "Batería de Condensadores",     	"CAP"	},
	[LOAD_TYPE_CCTV]						= { "Videovigilancia",			        "CCTV"	},
	[LOAD_TYPE_DATA_CENTER]					= { "Centro de Procesamiento de Datos",	"CPD"	},
	[LOAD_TYPE_PHOTOVOLTAIC]				= { "Fotovoltaica",                 	"PV"	},
	[LOAD_TYPE_POWER_OUTLET]				= { "Toma de Fuerza",	            	"TF"	},
	[LOAD_TYPE_GENERAL]						= { "Genérica",	                    	"GEN"	},
	[LOAD_TYPE_BACKUP_GENERATOR]			= { "Grupo Electrógeno",		        "GE"	},
	[LOAD_TYPE_HVAC]						= { "Unidad de Tratamiento de Aire",    "HVAC"	},
	[LOAD_TYPE_VARIABLE_FREQUENCY_DRIVE]	= { "Motor con Variador",	            "VFD"	},
	[LOAD_TYPE_MOTOR]						= { "Motor",	                    	"M"		},
	[LOAD_TYPE_OFFICE_WORKSTATIONS]			= { "Puesto de Trabajo",	            "PT"	},
	[LOAD_TYPE_EV_CHARGING]					= { "Recarga de Vehículo Eléctrico",	"EV"	},
	[LOAD_TYPE_RESERVE]						= { "Reserva",                  		"RES"	},
	[LOAD_TYPE_SPLIT]						= { "Climatización",	    	        "CLI"	},
	[LOAD_TYPE_AUXILIARY_SERVICES]			= { "Servicios Auxiliares",	        	"AUX"	},
};  

#endif