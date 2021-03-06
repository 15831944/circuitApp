/******************************************************************************
 *
 * Project:  OpenGIS Simple Features Reference Implementation
 * Purpose:  Implements OGRSQLiteViewLayer class, access to an existing spatialite view.
 * Author:   Even Rouault, <even dot rouault at mines dash paris dot org>
 *
 ******************************************************************************
 * Copyright (c) 2011-2013, Even Rouault <even dot rouault at mines-paris dot org>
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included
 * in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
 * OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 ****************************************************************************/

#include "cpl_port.h"
#include "ogr_sqlite.h"
#include "ogrsqliteutility.h"

#include <cstdlib>
#include <cstring>
#include <set>
#include <string>

#include "cpl_conv.h"
#include "cpl_error.h"
#include "cpl_string.h"
#include "ogr_core.h"
#include "ogr_feature.h"
#include "ogr_geometry.h"
#include "sqlite3.h"

CPL_CVSID("$Id: ogrsqliteviewlayer.cpp 722bc4c4c4f6ec3494bb2e4ee96b8742d72ac4c5 2018-02-14 22:51:15Z Kurt Schwehr $")

/************************************************************************/
/*                        OGRSQLiteViewLayer()                         */
/************************************************************************/

OGRSQLiteViewLayer::OGRSQLiteViewLayer( OGRSQLiteDataSource *poDSIn ) :
    bHasCheckedSpatialIndexTable(FALSE),
    eGeomFormat(OSGF_None),
    bHasSpatialIndex(FALSE),
    pszViewName(nullptr),
    pszEscapedTableName(nullptr),
    pszEscapedUnderlyingTableName(nullptr),
    bLayerDefnError(FALSE),
    poUnderlyingLayer(nullptr)
{
    poDS = poDSIn;
    iNextShapeId = 0;
    poFeatureDefn = nullptr;
}

/************************************************************************/
/*                        ~OGRSQLiteViewLayer()                        */
/************************************************************************/

OGRSQLiteViewLayer::~OGRSQLiteViewLayer()

{
    ClearStatement();
    CPLFree(pszViewName);
    CPLFree(pszEscapedTableName);
    CPLFree(pszEscapedUnderlyingTableName);
}

/************************************************************************/
/*                             Initialize()                             */
/************************************************************************/

CPLErr OGRSQLiteViewLayer::Initialize( const char *pszViewNameIn,
                                       const char *pszViewGeometry,
                                       const char *pszViewRowid,
                                       const char *pszUnderlyingTableName,
                                       const char *pszUnderlyingGeometryColumn)

{
    pszViewName = CPLStrdup(pszViewNameIn);
    SetDescription( pszViewName );

    osGeomColumn = pszViewGeometry;
    eGeomFormat = OSGF_SpatiaLite;

    CPLFree( pszFIDColumn );
    pszFIDColumn = CPLStrdup( pszViewRowid );

    osUnderlyingTableName = pszUnderlyingTableName;
    osUnderlyingGeometryColumn = pszUnderlyingGeometryColumn;
    poUnderlyingLayer = nullptr;

    pszEscapedTableName = CPLStrdup(SQLEscapeLiteral(pszViewName));
    pszEscapedUnderlyingTableName = CPLStrdup(SQLEscapeLiteral(pszUnderlyingTableName));

    return CE_None;
}

/************************************************************************/
/*                           GetLayerDefn()                             */
/************************************************************************/

OGRFeatureDefn* OGRSQLiteViewLayer::GetLayerDefn()
{
    if (poFeatureDefn)
        return poFeatureDefn;

    EstablishFeatureDefn();

    if (poFeatureDefn == nullptr)
    {
        bLayerDefnError = TRUE;

        poFeatureDefn = new OGRSQLiteFeatureDefn( pszViewName );
        poFeatureDefn->Reference();
    }

    return poFeatureDefn;
}

/************************************************************************/
/*                        GetUnderlyingLayer()                          */
/************************************************************************/

OGRSQLiteLayer* OGRSQLiteViewLayer::GetUnderlyingLayer()
{
    if( poUnderlyingLayer == nullptr )
    {
        if( strchr(osUnderlyingTableName, '(') == nullptr )
        {
            CPLString osNewUnderlyingTableName;
            osNewUnderlyingTableName.Printf("%s(%s)",
                                            osUnderlyingTableName.c_str(),
                                            osUnderlyingGeometryColumn.c_str());
            poUnderlyingLayer =
                (OGRSQLiteLayer*) poDS->GetLayerByNameNotVisible(osNewUnderlyingTableName);
        }
        if( poUnderlyingLayer == nullptr )
            poUnderlyingLayer =
                (OGRSQLiteLayer*) poDS->GetLayerByNameNotVisible(osUnderlyingTableName);
    }
    return poUnderlyingLayer;
}

/************************************************************************/
/*                            GetGeomType()                             */
/************************************************************************/

OGRwkbGeometryType OGRSQLiteViewLayer::GetGeomType()
{
    if (poFeatureDefn)
        return poFeatureDefn->GetGeomType();

    OGRSQLiteLayer* l_poUnderlyingLayer = GetUnderlyingLayer();
    if (l_poUnderlyingLayer)
        return l_poUnderlyingLayer->GetGeomType();

    return wkbUnknown;
}

/************************************************************************/
/*                         EstablishFeatureDefn()                       */
/************************************************************************/

CPLErr OGRSQLiteViewLayer::EstablishFeatureDefn()
{
    sqlite3 *hDB = poDS->GetDB();
    sqlite3_stmt *hColStmt = nullptr;

    OGRSQLiteLayer* l_poUnderlyingLayer = GetUnderlyingLayer();
    if (l_poUnderlyingLayer == nullptr)
    {
        CPLError(CE_Failure, CPLE_AppDefined,
                 "Cannot find underlying layer %s for view %s",
                 osUnderlyingTableName.c_str(), pszViewName);
        return CE_Failure;
    }
    if ( !l_poUnderlyingLayer->IsTableLayer() )
    {
        CPLError(CE_Failure, CPLE_AppDefined,
                 "Underlying layer %s for view %s is not a regular table",
                 osUnderlyingTableName.c_str(), pszViewName);
        return CE_Failure;
    }

    int nUnderlyingLayerGeomFieldIndex =
        l_poUnderlyingLayer->GetLayerDefn()->GetGeomFieldIndex(osUnderlyingGeometryColumn);
    if ( nUnderlyingLayerGeomFieldIndex < 0 )
    {
        CPLError(CE_Failure, CPLE_AppDefined,
                 "Underlying layer %s for view %s has not expected geometry column name %s",
                 osUnderlyingTableName.c_str(), pszViewName,
                 osUnderlyingGeometryColumn.c_str());
        return CE_Failure;
    }

    bHasSpatialIndex =
        l_poUnderlyingLayer->HasSpatialIndex(nUnderlyingLayerGeomFieldIndex);

/* -------------------------------------------------------------------- */
/*      Get the column definitions for this table.                      */
/* -------------------------------------------------------------------- */
    hColStmt = nullptr;
    const char *pszSQL =
        CPLSPrintf( "SELECT \"%s\", * FROM '%s' LIMIT 1",
                    SQLEscapeName(pszFIDColumn).c_str(),
                    pszEscapedTableName );

    int rc = sqlite3_prepare_v2( hDB, pszSQL, -1, &hColStmt, nullptr );
    if( rc != SQLITE_OK )
    {
        CPLError( CE_Failure, CPLE_AppDefined,
                  "Unable to query table %s for column definitions : %s.",
                  pszViewName, sqlite3_errmsg(hDB) );

        return CE_Failure;
    }

    rc = sqlite3_step( hColStmt );
    if ( rc != SQLITE_DONE && rc != SQLITE_ROW )
    {
        CPLError( CE_Failure, CPLE_AppDefined,
                  "In Initialize(): sqlite3_step(%s):\n  %s",
                  pszSQL, sqlite3_errmsg(hDB) );
        sqlite3_finalize( hColStmt );
        return CE_Failure;
    }

/* -------------------------------------------------------------------- */
/*      Collect the rest of the fields.                                 */
/* -------------------------------------------------------------------- */
    std::set<CPLString> aosGeomCols;
    std::set<CPLString> aosIgnoredCols;
    aosGeomCols.insert(osGeomColumn);
    BuildFeatureDefn( pszViewName, hColStmt, &aosGeomCols, aosIgnoredCols );
    sqlite3_finalize( hColStmt );

/* -------------------------------------------------------------------- */
/*      Set the properties of the geometry column.                      */
/* -------------------------------------------------------------------- */
    if( poFeatureDefn->GetGeomFieldCount() != 0 )
    {
        OGRSQLiteGeomFieldDefn* poSrcGeomFieldDefn =
            l_poUnderlyingLayer->myGetLayerDefn()->myGetGeomFieldDefn(nUnderlyingLayerGeomFieldIndex);
        OGRSQLiteGeomFieldDefn* poGeomFieldDefn =
            poFeatureDefn->myGetGeomFieldDefn(0);
        poGeomFieldDefn->SetType(poSrcGeomFieldDefn->GetType());
        poGeomFieldDefn->SetSpatialRef(poSrcGeomFieldDefn->GetSpatialRef());
        poGeomFieldDefn->nSRSId = poSrcGeomFieldDefn->nSRSId;
        if( eGeomFormat != OSGF_None )
            poGeomFieldDefn->eGeomFormat = eGeomFormat;
    }

    return CE_None;
}

/************************************************************************/
/*                           ResetStatement()                           */
/************************************************************************/

OGRErr OGRSQLiteViewLayer::ResetStatement()

{
    CPLString osSQL;

    ClearStatement();

    iNextShapeId = 0;

    osSQL.Printf( "SELECT \"%s\", * FROM '%s' %s",
                  SQLEscapeName(pszFIDColumn).c_str(),
                  pszEscapedTableName,
                  osWHERE.c_str() );

    const int rc =
        sqlite3_prepare_v2( poDS->GetDB(), osSQL, static_cast<int>(osSQL.size()),
                         &hStmt, nullptr );

    if( rc == SQLITE_OK )
    {
        return OGRERR_NONE;
    }

    CPLError( CE_Failure, CPLE_AppDefined,
              "In ResetStatement(): sqlite3_prepare_v2(%s):\n  %s",
              osSQL.c_str(), sqlite3_errmsg(poDS->GetDB()) );
    hStmt = nullptr;
    return OGRERR_FAILURE;
}

/************************************************************************/
/*                           GetNextFeature()                           */
/************************************************************************/

OGRFeature *OGRSQLiteViewLayer::GetNextFeature()

{
    if (HasLayerDefnError())
        return nullptr;

    return OGRSQLiteLayer::GetNextFeature();
}

/************************************************************************/
/*                             GetFeature()                             */
/************************************************************************/

OGRFeature *OGRSQLiteViewLayer::GetFeature( GIntBig nFeatureId )

{
    if (HasLayerDefnError())
        return nullptr;

/* -------------------------------------------------------------------- */
/*      If we don't have an explicit FID column, just read through      */
/*      the result set iteratively to find our target.                  */
/* -------------------------------------------------------------------- */
    if( pszFIDColumn == nullptr )
        return OGRSQLiteLayer::GetFeature( nFeatureId );

/* -------------------------------------------------------------------- */
/*      Setup explicit query statement to fetch the record we want.     */
/* -------------------------------------------------------------------- */
    CPLString osSQL;

    ClearStatement();

    iNextShapeId = nFeatureId;

    osSQL.Printf( "SELECT \"%s\", * FROM '%s' WHERE \"%s\" = %d",
                  SQLEscapeName(pszFIDColumn).c_str(),
                  pszEscapedTableName,
                  SQLEscapeName(pszFIDColumn).c_str(),
                  (int) nFeatureId );

    CPLDebug( "OGR_SQLITE", "exec(%s)", osSQL.c_str() );

    const int rc =
        sqlite3_prepare_v2( poDS->GetDB(), osSQL, static_cast<int>(osSQL.size()),
                         &hStmt, nullptr );
    if( rc != SQLITE_OK )
    {
        CPLError( CE_Failure, CPLE_AppDefined,
                  "In GetFeature(): sqlite3_prepare_v2(%s):\n  %s",
                  osSQL.c_str(), sqlite3_errmsg(poDS->GetDB()) );

        return nullptr;
    }
/* -------------------------------------------------------------------- */
/*      Get the feature if possible.                                    */
/* -------------------------------------------------------------------- */
    OGRFeature *poFeature = GetNextRawFeature();

    ResetReading();

    return poFeature;
}

/************************************************************************/
/*                         SetAttributeFilter()                         */
/************************************************************************/

OGRErr OGRSQLiteViewLayer::SetAttributeFilter( const char *pszQuery )

{
    if( pszQuery == nullptr )
        osQuery = "";
    else
        osQuery = pszQuery;

    BuildWhere();

    ResetReading();

    return OGRERR_NONE;
}

/************************************************************************/
/*                          SetSpatialFilter()                          */
/************************************************************************/

void OGRSQLiteViewLayer::SetSpatialFilter( OGRGeometry * poGeomIn )

{
    if( InstallFilter( poGeomIn ) )
    {
        BuildWhere();

        ResetReading();
    }
}

/************************************************************************/
/*                           GetSpatialWhere()                          */
/************************************************************************/

CPLString OGRSQLiteViewLayer::GetSpatialWhere(int iGeomCol,
                                              OGRGeometry* poFilterGeom)
{
    if (HasLayerDefnError() || poFeatureDefn == nullptr ||
        iGeomCol < 0 || iGeomCol >= poFeatureDefn->GetGeomFieldCount())
        return "";

    if( poFilterGeom != nullptr && bHasSpatialIndex )
    {
        OGREnvelope  sEnvelope;

        poFilterGeom->getEnvelope( &sEnvelope );

        /* We first check that the spatial index table exists */
        if (!bHasCheckedSpatialIndexTable)
        {
            bHasCheckedSpatialIndexTable = TRUE;
            char **papszResult = nullptr;
            int nRowCount = 0;
            int nColCount = 0;
            char *pszErrMsg = nullptr;

            CPLString osSQL;
            osSQL.Printf("SELECT name FROM sqlite_master "
                        "WHERE name='idx_%s_%s'",
                        pszEscapedUnderlyingTableName,
                        SQLEscapeLiteral(osUnderlyingGeometryColumn).c_str());

            int  rc = sqlite3_get_table( poDS->GetDB(), osSQL.c_str(),
                                        &papszResult, &nRowCount,
                                        &nColCount, &pszErrMsg );

            if( rc != SQLITE_OK )
            {
                CPLError( CE_Failure, CPLE_AppDefined, "Error: %s",
                        pszErrMsg );
                sqlite3_free( pszErrMsg );
                bHasSpatialIndex = FALSE;
            }
            else
            {
                if (nRowCount != 1)
                {
                    bHasSpatialIndex = FALSE;
                }

                sqlite3_free_table(papszResult);
            }
        }

        if (bHasSpatialIndex)
        {
            return FormatSpatialFilterFromRTree(poFilterGeom,
                CPLSPrintf("\"%s\"", SQLEscapeName(pszFIDColumn).c_str()),
                pszEscapedUnderlyingTableName,
                SQLEscapeLiteral(osUnderlyingGeometryColumn).c_str());
        }
        else
        {
            CPLDebug("SQLITE", "Count not find idx_%s_%s layer. Disabling spatial index",
                     pszEscapedUnderlyingTableName, osUnderlyingGeometryColumn.c_str());
        }
    }

    if( poFilterGeom != nullptr && poDS->IsSpatialiteLoaded() )
    {
        return FormatSpatialFilterFromMBR(poFilterGeom,
            SQLEscapeName(poFeatureDefn->GetGeomFieldDefn(iGeomCol)->GetNameRef()).c_str());
    }

    return "";
}

/************************************************************************/
/*                             BuildWhere()                             */
/*                                                                      */
/*      Build the WHERE statement appropriate to the current set of     */
/*      criteria (spatial and attribute queries).                       */
/************************************************************************/

void OGRSQLiteViewLayer::BuildWhere()

{
    osWHERE = "";

    CPLString osSpatialWHERE = GetSpatialWhere(m_iGeomFieldFilter,
                                               m_poFilterGeom);
    if (!osSpatialWHERE.empty())
    {
        osWHERE = "WHERE ";
        osWHERE += osSpatialWHERE;
    }

    if( !osQuery.empty() )
    {
        if( osWHERE.empty() )
        {
            osWHERE = "WHERE ";
            osWHERE += osQuery;
        }
        else
        {
            osWHERE += " AND (";
            osWHERE += osQuery;
            osWHERE += ")";
        }
    }
}

/************************************************************************/
/*                           TestCapability()                           */
/************************************************************************/

int OGRSQLiteViewLayer::TestCapability( const char * pszCap )

{
    if (HasLayerDefnError())
        return FALSE;

    if (EQUAL(pszCap,OLCFastFeatureCount))
        return m_poFilterGeom == nullptr || osGeomColumn.empty() ||
               bHasSpatialIndex;

    else if (EQUAL(pszCap,OLCFastSpatialFilter))
        return bHasSpatialIndex;

    else
        return OGRSQLiteLayer::TestCapability( pszCap );
}

/************************************************************************/
/*                          GetFeatureCount()                           */
/*                                                                      */
/*      If a spatial filter is in effect, we turn control over to       */
/*      the generic counter.  Otherwise we return the total count.      */
/*      Eventually we should consider implementing a more efficient     */
/*      way of counting features matching a spatial query.              */
/************************************************************************/

GIntBig OGRSQLiteViewLayer::GetFeatureCount( int bForce )

{
    if (HasLayerDefnError())
        return 0;

    if( !TestCapability(OLCFastFeatureCount) )
        return OGRSQLiteLayer::GetFeatureCount( bForce );

/* -------------------------------------------------------------------- */
/*      Form count SQL.                                                 */
/* -------------------------------------------------------------------- */
    const char *pszSQL =
        CPLSPrintf( "SELECT count(*) FROM '%s' %s",
                    pszEscapedTableName, osWHERE.c_str() );

/* -------------------------------------------------------------------- */
/*      Execute.                                                        */
/* -------------------------------------------------------------------- */
    char **papszResult, *pszErrMsg;
    int nRowCount, nColCount;
    int nResult = -1;

    if( sqlite3_get_table( poDS->GetDB(), pszSQL, &papszResult,
                           &nColCount, &nRowCount, &pszErrMsg ) != SQLITE_OK )
        return -1;

    if( nRowCount == 1 && nColCount == 1 )
        nResult = atoi(papszResult[1]);

    sqlite3_free_table( papszResult );

    return nResult;
}
