CREATE OR REPLACE FUNCTION spgist_geom_config(internal, internal)
	RETURNS void
	AS '$libdir/postgis-2.1' ,'spgist_geom_config'
	LANGUAGE 'c';

CREATE OR REPLACE FUNCTION spgist_geom_choose(internal, internal)
	RETURNS void
	AS '$libdir/postgis-2.1' ,'spgist_geom_choose'
	LANGUAGE 'c';

CREATE OR REPLACE FUNCTION spgist_geom_picksplit(internal, internal)
	RETURNS void
	AS '$libdir/postgis-2.1' ,'spgist_geom_picksplit'
	LANGUAGE 'c';

CREATE OR REPLACE FUNCTION spgist_geom_inner_consistent(internal, internal)
	RETURNS void
	AS '$libdir/postgis-2.1' ,'spgist_geom_inner_consistent'
	LANGUAGE 'c';

CREATE OR REPLACE FUNCTION spgist_geom_leaf_consistent(internal, internal)
	RETURNS boolean
	AS '$libdir/postgis-2.1' ,'spgist_geom_leaf_consistent'
	LANGUAGE 'c';

CREATE OPERATOR CLASS spgist_geometry_ops_2d
        FOR TYPE geometry
        USING SPGiST
        AS OPERATOR 1 <<,
           OPERATOR 5 >>,
           OPERATOR 6 ~=,
           OPERATOR 8 @,
           OPERATOR 10 <<|,
           OPERATOR 11 |>>,
           FUNCTION 1 spgist_geom_config(internal, internal),
           FUNCTION 2 spgist_geom_choose(internal, internal),
           FUNCTION 3 spgist_geom_picksplit(internal, internal),
           FUNCTION 4 spgist_geom_inner_consistent(internal, internal),
           FUNCTION 5 spgist_geom_leaf_consistent(internal, internal)

