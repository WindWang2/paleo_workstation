from contextlib import closing
import importlib.util
import json
from pathlib import Path
import sqlite3
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
def module(name):
    spec = importlib.util.spec_from_file_location(name, ROOT / 'tools' / (name + '.py'))
    result = importlib.util.module_from_spec(spec); spec.loader.exec_module(result); return result

class FaciesAuditTest(unittest.TestCase):
    def test_prediction_is_independent_from_interpretation(self):
        audit = module('audit_well_facies')
        system = [dict(code=1, name='channel', facies='delta', subfacies='plain', microfacies='channel'),
                  dict(code=2, name='bay', facies='delta', subfacies='plain', microfacies='bay')]
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory); project = root / 'project'
            catalog = project / 'artifacts/catalog/catalog.sqlite'; catalog.parent.mkdir(parents=True)
            with closing(sqlite3.connect(catalog)) as db, db:
                db.executescript('create table assets(id,type); create table versions(id,asset_id,path,extra_json,version_number); create table entities(entity_type,name);')
                db.execute('insert into assets values(?,?)', ('schema','facies_schema'))
                db.execute('insert into versions values(?,?,?,?,?)', ('s','schema','',json.dumps(dict(horizon='D61',facies=system[:1])),1))
                db.execute('insert into entities values(?,?)', ('well','A1'))
            table = project / 'artifacts/layers/well-log-attributes.gpkg'; table.parent.mkdir(parents=True)
            with closing(sqlite3.connect(table)) as db, db:
                db.executescript("create table gpkg_contents(table_name,data_type); insert into gpkg_contents values('attributes','features'); create table attributes(well_name,horizon,facies_code,facies,sub_facies,predicted_facies);")
                db.execute('insert into attributes values(?,?,?,?,?,?)', ('A1','D61',1,'delta','plain','bay'))
            result = audit.audit([root], None, system, system)
            self.assertTrue(result['ok']); self.assertEqual(len(result['warnings']),1)
            self.assertEqual(result['projects'][0]['predicted_wells'], ['A1'])
            with closing(sqlite3.connect(table)) as db, db: db.execute("update attributes set predicted_facies='unknown'")
            result = audit.audit([root], None, system, system)
            self.assertFalse(result['ok']); self.assertEqual(len(result['issues']),1)

try:
    import numpy as np
    import shapely
    import shapefile
except ImportError:
    shapely = None

@unittest.skipIf(shapely is None or not hasattr(shapely, 'coverage_simplify'), 'optional Shapely 2.1 source preparation dependencies unavailable')
class CoverageTest(unittest.TestCase):
    def test_shared_edges_holes_and_paired_file_attributes(self):
        tool = module('simplify_historical_maps')
        from shapely.geometry import Polygon, mapping
        # Shared zigzag and a hole must survive as an edge-matched coverage.
        left = Polygon([(0,0),(100,0),(100.1,25),(100,50),(100.1,75),(100,100),(0,100)],
                       holes=[[(20,20),(20,40),(40,40),(40,20)]])
        right = Polygon([(100,0),(200,0),(200,100),(100,100),(100.1,75),(100,50),(100.1,25)])
        geometries = np.array([left,right], dtype=object)
        simple, report = tool.simplify(geometries, 2.)
        self.assertTrue(shapely.coverage_is_valid(simple))
        self.assertEqual(tool.topology_counts(simple[0]), (1,1))
        self.assertLess(report['vertices_after'], report['vertices_before'])
        with tempfile.TemporaryDirectory() as directory:
            source = Path(directory)/'original.gpkg'; output = Path(directory)/'new.gpkg'
            with closing(sqlite3.connect(source)) as db, db:
                db.executescript('create table gpkg_geometry_columns(table_name,column_name,srs_id); create table gpkg_contents(table_name,min_x,min_y,max_x,max_y); create table facies(fid integer primary key,geom blob,name text,area_m2 real);')
                db.execute("insert into gpkg_geometry_columns values('facies','geom',32649)")
                db.execute("insert into gpkg_contents values('facies',0,0,200,100)")
                for i,g in enumerate(geometries): db.execute('insert into facies values(?,?,?,?)',(i+1,tool.gpkg_blob(g,32649),'class'+str(i),g.area))
            shape_path = source.with_suffix('.shp')
            with shapefile.Writer(str(shape_path), shapeType=shapefile.POLYGON, encoding='utf-8') as writer:
                writer.field('name','C'); writer.field('area_m2','N',18,4)
                for i,g in enumerate(geometries): writer.shape(mapping(shapely.orient_polygons(g,exterior_cw=True))); writer.record('class'+str(i),g.area)
            tool.write_package(source,output,simple); tool.write_shape(shape_path,output.with_suffix('.shp'),simple)
            tool.validate_pair(source,output,shape_path,output.with_suffix('.shp'))
            self.assertEqual(tool.read_package(output)[2],32649)

if __name__ == '__main__': unittest.main()
