/******************************************************************************
 * $Id$
 *
 * Project:  MapServer
 * Purpose:  Scale object rendering.
 * Author:   Steve Lime and the MapServer team.
 *
 ******************************************************************************
 * Copyright (c) 1996-2005 Regents of the University of Minnesota.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies of this Software or works derived from this Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
 * OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 ****************************************************************************/

#include "mapserver.h"



#define VMARGIN 3 /* buffer around the scalebar */
#define HMARGIN 3
#define VSPACING .8 /* spacing (% of font height) between scalebar and text */
#define VSLOP 5 /* makes things fit a bit better vertically */

/*
** Match this with with unit enumerations is mapserver.h
*/
static char *unitText[9]= {"in", "ft", "mi", "m", "km", "dd", "??", "??", "NM"}; /* MS_PIXEL and MS_PERCENTAGE not used */
double inchesPerUnit[9]= {1, 12, 63360.0, 39.3701, 39370.1, 4374754, 1, 1, 72913.3858 };

static double roundInterval(double d)
{
  if(d<.001)
    return(MS_NINT(d*10000)/10000.0);
  if(d<.01)
    return(MS_NINT(d*1000)/1000.0);
  if(d<.1)
    return(MS_NINT(d*100)/100.0);
  if(d<1)
    return(MS_NINT(d*10)/10.0);
  if(d<100)
    return(MS_NINT(d));
  if(d<1000)
    return(MS_NINT(d/10) * 10);
  if(d<10000)
    return(MS_NINT(d/100) * 100);
  if(d<100000)
    return(MS_NINT(d/1000) * 1000);
  if(d<1000000)
    return(MS_NINT(d/10000) * 10000);
  if(d<10000000)
    return(MS_NINT(d/100000) * 100000);
  if(d<100000000)
    return(MS_NINT(d/1000000) * 1000000);

  return(-1);
}

static double roundInterval2(double d)
{
  double magnitude, ratio;
      
  magnitude = floor(log10(d));
  ratio = d / pow(10, magnitude);
  if (ratio < 1.5) ratio = 1;
  else if (ratio < 4) ratio = 2;
  else if (ratio < 8) ratio = 5;
  else ratio = 10;

  return ratio * pow(10, magnitude);
}

/*
** Calculate the approximate scale based on a few parameters. Note that this assumes the scale is
** the same in the x direction as in the y direction, so run msAdjustExtent(...) first.
*/
int msCalculateScale(rectObj extent, int units, int width, int height, int pixeladjustment,  double resolution, double *scale)
{
  double md, gd, center_y;

  /* if((extent.maxx == extent.minx) || (extent.maxy == extent.miny))   */
  if(!MS_VALID_EXTENT(extent)) {
    msSetError(MS_MISCERR, "Invalid image extent, minx=%lf, miny=%lf, maxx=%lf, maxy=%lf.", "msCalculateScale()", extent.minx, extent.miny, extent.maxx, extent.maxy);
    return(MS_FAILURE);
  }

  if((width <= 0) || (height <= 0)) {
    msSetError(MS_MISCERR, "Invalid image width or height.", "msCalculateScale()");
    return(MS_FAILURE);
  }

  switch (units) {
    case(MS_DD):
    case(MS_METERS):
    case(MS_KILOMETERS):
    case(MS_MILES):
    case(MS_NAUTICALMILES):
    case(MS_INCHES):
    case(MS_FEET):
      center_y = (extent.miny+extent.maxy)/2.0;
      md = (width - pixeladjustment)/(resolution*msInchesPerUnit(units, center_y)); /* remember, we use a pixel-center to pixel-center extent, hence the width-1 */
      gd = extent.maxx - extent.minx;
      *scale = gd/md;
      break;
    default:
      *scale = -1; /* this is not an error */
      break;
  }

  return(MS_SUCCESS);
}

double msInchesPerUnit(int units, double center_lat)
{
  double lat_adj = 1.0, ipu = 1.0;

  switch (units) {
    case(MS_METERS):
    case(MS_KILOMETERS):
    case(MS_MILES):
    case(MS_NAUTICALMILES):
    case(MS_INCHES):
    case(MS_FEET):
      ipu = inchesPerUnit[units];
      break;
    case(MS_DD):
      /* With geographical (DD) coordinates, we adjust the inchesPerUnit
       * based on the latitude of the center of the view. For this we assume
       * we have a perfect sphere and just use cos(lat) in our calculation.
       */
#ifdef ENABLE_VARIABLE_INCHES_PER_DEGREE
      if (center_lat != 0.0) {
        double cos_lat;
        cos_lat = cos(MS_PI*center_lat/180.0);
        lat_adj = sqrt(1+cos_lat*cos_lat)/sqrt(2.0);
      }
#endif
      ipu = inchesPerUnit[units]*lat_adj;
      break;
    default:
      break;
  }

  return ipu;
}


#define X_STEP_SIZE 5

/* TODO : the will be msDrawScalebarGD */
imageObj *msDrawScalebar(mapObj *map)
{
  int status;
  char label[32];
  double i, msx, resolutionfactor, strokeWidth, vSlop;
  int j;
  int isx, sx, sy, ox, oy, state, dsx, scalebarWidth, scalebarHeight, hMargin, vMargin, units;
  pointObj p;
  rectObj r;
  imageObj      *image = NULL;
  double fontWidth, fontHeight;
  outputFormatObj *format = NULL;
  strokeStyleObj strokeStyle;
  shapeObj shape;
  lineObj line;
  pointObj points[5];
  rendererVTableObj *renderer;

  strokeStyle.patternlength=0;

  if(map->units == -1) {
    msSetError(MS_MISCERR, "Map units not set.", "msDrawScalebar()");
    return(NULL);
  }

  renderer = MS_MAP_RENDERER(map);
  if(!renderer || !(MS_MAP_RENDERER(map)->supports_pixel_buffer || MS_MAP_RENDERER(map)->supports_svg)) {
    msSetError(MS_MISCERR, "Outputformat not supported for scalebar", "msDrawScalebar()");
    return(NULL);
  }

  resolutionfactor = map->resolution/map->defresolution;
  scalebarWidth = MS_NINT(resolutionfactor * map->scalebar.width);
  scalebarHeight = MS_NINT(resolutionfactor * map->scalebar.height);
  hMargin = MS_NINT(resolutionfactor * HMARGIN);
  vMargin = MS_NINT(resolutionfactor * VMARGIN);
  strokeWidth = resolutionfactor;
  vSlop = resolutionfactor * VSLOP;
  units = map->scalebar.units;

  /*
   *  A string containing the ten decimal digits is rendered to compute an average cell size
   *  for each number, which is used later to place labels on the scalebar.
   */

  if(msGetLabelSize(map,&map->scalebar.label,"0123456789",map->scalebar.label.size,&r,NULL) != MS_SUCCESS) {
    return NULL;
  }
  fontWidth = (r.maxx-r.minx)/10.0 * resolutionfactor;
  fontHeight = (r.maxy -r.miny) * resolutionfactor;

  map->cellsize = msAdjustExtent(&(map->extent), map->width, map->height, map->pixeladjustment);
  status = msCalculateScale(map->extent, map->units, map->width, map->height, map->pixeladjustment, map->resolution, &map->scaledenom);
  if(status != MS_SUCCESS) {
    return(NULL);
  }
  dsx = (scalebarWidth - 2*hMargin);
  do {
    msx = (map->cellsize * dsx)/(msInchesPerUnit(units,0)/msInchesPerUnit(map->units,0));
    i = roundInterval2(msx/map->scalebar.intervals);
    if (units == MS_METERS && i >= 1000) {
        units = MS_KILOMETERS;
        i /= 1000;
    }
    else if (units == MS_KILOMETERS && i <= 0.001) {
        units = MS_METERS;
        i *= 1000;
    }
    snprintf(label, sizeof(label), "%g", map->scalebar.intervals*i); /* last label */
    isx = MS_NINT((i/(msInchesPerUnit(map->units,0)/msInchesPerUnit(units,0)))/map->cellsize);
    sx = (map->scalebar.intervals*isx) + MS_NINT((1.5 + strlen(label)/2.0 + strlen(unitText[units]))*fontWidth);

    if(sx <= (scalebarWidth - 2*hMargin)) break; /* it will fit */

    dsx -= X_STEP_SIZE; /* change the desired size in hopes that it will fit in user supplied width */
  } while(1);

  sy = MS_NINT((2*vMargin) + MS_NINT(VSPACING*fontHeight) + fontHeight + scalebarHeight - vSlop);

  /* For embed scalebars set scalebar width to the content width */
  if (map->scalebar.status == MS_EMBED) {
      scalebarWidth = sx + 2*hMargin;
  }

  /*Ensure we have an image format representing the options for the scalebar.*/
  msApplyOutputFormat( &format, map->outputformat,
                       map->scalebar.transparent,
                       map->scalebar.interlace,
                       MS_NOOVERRIDE );

  if(map->scalebar.transparent == MS_OFF) {
    if(!MS_VALID_COLOR(map->scalebar.imagecolor))
      MS_INIT_COLOR(map->scalebar.imagecolor,255,255,255,255);
  }
  image = msImageCreate(scalebarWidth, sy, format,
                        map->web.imagepath, map->web.imageurl, map->resolution, map->defresolution, &map->scalebar.imagecolor);

  /* drop this reference to output format */
  msApplyOutputFormat( &format, NULL,
                       MS_NOOVERRIDE, MS_NOOVERRIDE, MS_NOOVERRIDE );

  /* did we succeed in creating the image? */
  if(!image) {
    msSetError(MS_MISCERR, "Unable to initialize image.", "msDrawScalebar()");
    return NULL;
  }

  switch(map->scalebar.align) {
    case(MS_ALIGN_LEFT):
      ox = hMargin;
      break;
    case(MS_ALIGN_RIGHT):
      ox = MS_NINT((scalebarWidth - sx) + fontWidth);
      break;
    default:
      ox = MS_NINT((scalebarWidth - sx)/2.0 + fontWidth/2.0); /* center the computed scalebar */
  }
  oy = vMargin;

  switch(map->scalebar.style) {
    case(0): {

      line.numpoints = 5;
      line.point = points;
      shape.line = &line;
      shape.numlines = 1;
      if(MS_VALID_COLOR(map->scalebar.color)) {
        INIT_STROKE_STYLE(strokeStyle);
        strokeStyle.color = &map->scalebar.outlinecolor;
        strokeStyle.color->alpha = 255;
        strokeStyle.width = strokeWidth;
      }
      map->scalebar.backgroundcolor.alpha = 255;
      map->scalebar.color.alpha = 255;
      state = 1; /* 1 means filled */
      for(j=0; j<map->scalebar.intervals; j++) {
        points[0].x = points[4].x = points[3].x = ox + j*isx + 0.5;
        points[0].y = points[4].y = points[1].y = oy + 0.5;
        points[1].x = points[2].x = ox + (j+1)*isx + 0.5;
        points[2].y = points[3].y = oy + scalebarHeight + 0.5;
        if(state == 1 && MS_VALID_COLOR(map->scalebar.color))
          renderer->renderPolygon(image,&shape,&map->scalebar.color);
        else if(MS_VALID_COLOR(map->scalebar.backgroundcolor))
          renderer->renderPolygon(image,&shape,&map->scalebar.backgroundcolor);

        if(strokeStyle.color)
          renderer->renderLine(image,&shape,&strokeStyle);

        sprintf(label, "%g", j*i);
        map->scalebar.label.position = MS_CC;
        p.x = ox + j*isx; /* + MS_NINT(fontPtr->w/2); */
        p.y = oy + scalebarHeight + MS_NINT(VSPACING*fontHeight);
        msDrawLabel(map,image,p,label,&map->scalebar.label,resolutionfactor);
        state = -state;
      }
      sprintf(label, "%g", j*i);
      ox = ox + j*isx - MS_NINT((strlen(label)*fontWidth)/2.0);
      sprintf(label, "%g %s", j*i, unitText[units]);
      map->scalebar.label.position = MS_CR;
      p.x = ox; /* + MS_NINT(fontPtr->w/2); */
      p.y = oy + scalebarHeight + MS_NINT(VSPACING*fontHeight);
      msDrawLabel(map,image,p,label,&map->scalebar.label,resolutionfactor);

      break;
    }
    case(1): {
      line.numpoints = 2;
      line.point = points;
      shape.line = &line;
      shape.numlines = 1;
      if(MS_VALID_COLOR(map->scalebar.color)) {
        strokeStyle.width = strokeWidth;
        strokeStyle.color = &map->scalebar.color;
      }

      points[0].y = points[1].y = oy;
      points[0].x = ox;
      points[1].x = ox + isx*map->scalebar.intervals;
      renderer->renderLine(image,&shape,&strokeStyle);

      points[0].y = oy;
      points[1].y = oy + scalebarHeight;
      p.y = oy + scalebarHeight + MS_NINT(VSPACING*fontHeight);
      for(j=0; j<=map->scalebar.intervals; j++) {
        points[0].x = points[1].x = ox + j*isx;
        renderer->renderLine(image,&shape,&strokeStyle);

        sprintf(label, "%g", j*i);
        if(j!=map->scalebar.intervals) {
          map->scalebar.label.position = MS_CC;
          p.x = ox + j*isx; /* + MS_NINT(fontPtr->w/2); */
        } else {
          sprintf(label, "%g %s", j*i, unitText[units]);
          map->scalebar.label.position = MS_CR;
          p.x = ox + j*isx - MS_NINT((strlen(label)*fontWidth)/2.0);
        }
        msDrawLabel(map,image,p,label,&map->scalebar.label,resolutionfactor);
      }
      break;
    }
    default:
      msSetError(MS_MISCERR, "Unsupported scalebar style.", "msDrawScalebar()");
      return(NULL);
  }
  return(image);

}

int msEmbedScalebar(mapObj *map, imageObj *img)
{
  int l,index,s;
  pointObj point;
  imageObj *image = NULL;
  rendererVTableObj *renderer;
  symbolObj *embededSymbol;
  char* imageType = NULL;

  index = msGetSymbolIndex(&(map->symbolset), "scalebar", MS_FALSE);
  if(index != -1)
    msRemoveSymbol(&(map->symbolset), index); /* remove cached symbol in case the function is called multiple
                      times with different zoom levels */

  if((embededSymbol=msGrowSymbolSet(&map->symbolset)) == NULL)
    return MS_FAILURE;

  s = map->symbolset.numsymbols;
  map->symbolset.numsymbols++;

  if(!MS_RENDERER_PLUGIN(map->outputformat) || !MS_MAP_RENDERER(map)->supports_pixel_buffer) {
    imageType = msStrdup(map->imagetype); /* save format */
    if MS_DRIVER_CAIRO(map->outputformat) {
#ifdef USE_SVG_CAIRO
      map->outputformat = msSelectOutputFormat( map, "svg" );
#else
      map->outputformat = msSelectOutputFormat( map, "cairopng" );
#endif
    }
    else
      map->outputformat = msSelectOutputFormat( map, "png" );
    
    msInitializeRendererVTable(map->outputformat);
  }
  renderer = MS_MAP_RENDERER(map);

  image = msDrawScalebar(map);

  if (imageType) {
    map->outputformat = msSelectOutputFormat( map, imageType ); /* restore format */
    msFree(imageType);
  }

  if(!image) {
    return MS_FAILURE;
  }

  /* intialize a few things */
  embededSymbol->name = msStrdup("scalebar");

  if (!strcasecmp(image->format->driver,"cairo/svg")) {
    int size;
    char* svg_text;
    embededSymbol->type = MS_SYMBOL_SVG;
    svg_text = msSaveImageBuffer(image, &size, map->outputformat);
    msFreeImage( image );
    if (!svg_text)
      return MS_FAILURE;
    embededSymbol->svg_text = msSmallMalloc(size + 1);
    memcpy(embededSymbol->svg_text, svg_text, size);
    embededSymbol->svg_text[size] = 0;
    msFree(svg_text);
    if(MS_SUCCESS != msPreloadSVGSymbol(embededSymbol))
      return MS_FAILURE;
  }
  else {
    embededSymbol->pixmap_buffer = calloc(1,sizeof(rasterBufferObj));
    MS_CHECK_ALLOC(embededSymbol->pixmap_buffer, sizeof(rasterBufferObj), MS_FAILURE);

    if(MS_SUCCESS != renderer->getRasterBufferCopy(image,embededSymbol->pixmap_buffer)) {
      msFreeImage( image );
      return MS_FAILURE;
    }
    msFreeImage( image );
    embededSymbol->type = MS_SYMBOL_PIXMAP;
    embededSymbol->sizex = embededSymbol->pixmap_buffer->width;
    embededSymbol->sizey = embededSymbol->pixmap_buffer->height;
  }
  if(map->scalebar.transparent) {
    embededSymbol->transparent = MS_TRUE;
    embededSymbol->transparentcolor = 0;
  }

  switch(map->scalebar.position) {
    case(MS_LL):
      point.x = MS_NINT(embededSymbol->sizex/2.0);
      point.y = map->height - MS_NINT(embededSymbol->sizey/2.0);
      break;
    case(MS_LR):
      point.x = map->width - MS_NINT(embededSymbol->sizex/2.0);
      point.y = map->height - MS_NINT(embededSymbol->sizey/2.0);
      break;
    case(MS_LC):
      point.x = MS_NINT(map->width/2.0);
      point.y = map->height - MS_NINT(embededSymbol->sizey/2.0);
      break;
    case(MS_UR):
      point.x = map->width - MS_NINT(embededSymbol->sizex/2.0);
      point.y = MS_NINT(embededSymbol->sizey/2.0);
      break;
    case(MS_UL):
      point.x = MS_NINT(embededSymbol->sizex/2.0);
      point.y = MS_NINT(embededSymbol->sizey/2.0);
      break;
    case(MS_UC):
      point.x = MS_NINT(map->width/2.0);
      point.y = MS_NINT(embededSymbol->sizey/2.0);
      break;
  }

  l = msGetLayerIndex(map, "__embed__scalebar");
  if(l == -1) {
    if (msGrowMapLayers(map) == NULL)
      return(-1);
    l = map->numlayers;
    map->numlayers++;
    if(initLayer((GET_LAYER(map, l)), map) == -1) return(-1);
    GET_LAYER(map, l)->name = msStrdup("__embed__scalebar");
    GET_LAYER(map, l)->type = MS_LAYER_POINT;

    if (msGrowLayerClasses( GET_LAYER(map, l) ) == NULL)
      return(-1);

    if(initClass(GET_LAYER(map, l)->class[0]) == -1) return(-1);
    GET_LAYER(map, l)->numclasses = 1; /* so we make sure to free it */

    /* update the layer order list with the layer's index. */
    map->layerorder[l] = l;
  }

  GET_LAYER(map, l)->status = MS_ON;
  GET_LAYER(map, l)->scalefactor = 1; /* no need to magnify symbol */
  if(map->scalebar.postlabelcache) { /* add it directly to the image */
    if(msMaybeAllocateClassStyle(GET_LAYER(map, l)->class[0], 0)==MS_FAILURE) return MS_FAILURE;
    GET_LAYER(map, l)->class[0]->styles[0]->symbol = s;
    msDrawMarkerSymbol(&map->symbolset, img, &point, GET_LAYER(map, l)->class[0]->styles[0], 1.0);
  } else {
    if(!GET_LAYER(map, l)->class[0]->labels) {
      if(msGrowClassLabels(GET_LAYER(map, l)->class[0]) == NULL) return MS_FAILURE;
      initLabel(GET_LAYER(map, l)->class[0]->labels[0]);
      GET_LAYER(map, l)->class[0]->numlabels = 1;
      GET_LAYER(map, l)->class[0]->labels[0]->force = MS_TRUE;
      GET_LAYER(map, l)->class[0]->labels[0]->size = MS_MEDIUM; /* must set a size to have a valid label definition */
      GET_LAYER(map, l)->class[0]->labels[0]->priority = MS_MAX_LABEL_PRIORITY;
      GET_LAYER(map, l)->class[0]->labels[0]->annotext = NULL;
    }
    if(GET_LAYER(map, l)->class[0]->labels[0]->numstyles == 0) {
      if(msGrowLabelStyles(GET_LAYER(map,l)->class[0]->labels[0]) == NULL)
        return(MS_FAILURE);
      GET_LAYER(map,l)->class[0]->labels[0]->numstyles = 1;
      initStyle(GET_LAYER(map,l)->class[0]->labels[0]->styles[0]);
      GET_LAYER(map,l)->class[0]->labels[0]->styles[0]->_geomtransform.type = MS_GEOMTRANSFORM_LABELPOINT;
    }
    GET_LAYER(map,l)->class[0]->labels[0]->styles[0]->symbol = s;
    msAddLabel(map, GET_LAYER(map, l)->class[0]->labels[0], l, 0, NULL, &point, NULL, -1);
  }


  /* Mark layer as deleted so that it doesn't interfere with html legends or with saving maps */
  GET_LAYER(map, l)->status = MS_DELETE;

  return(0);
}

/************************************************************************/
/* These two functions are used in PHP/Mapscript and Swig/Mapscript     */
/************************************************************************/

/************************************************************************/
/*  double GetDeltaExtentsUsingScale(double scale, int units,           */
/*                                   double centerLat, int width,       */
/*                                   double resolution)                 */
/*                                                                      */
/*      Utility function to return the maximum extent using the         */
/*      scale and the width of the image.                               */
/*                                                                      */
/*      Base on the function msCalculateScale (mapscale.c)              */
/************************************************************************/
double GetDeltaExtentsUsingScale(double scale, int units, double centerLat, int width, double resolution)
{
  double md = 0.0;
  double dfDelta = -1.0;

  if (scale <= 0 || width <=0)
    return -1;

  switch (units) {
    case(MS_DD):
    case(MS_METERS):
    case(MS_KILOMETERS):
    case(MS_MILES):
    case(MS_NAUTICALMILES):
    case(MS_INCHES):
    case(MS_FEET):
      /* remember, we use a pixel-center to pixel-center extent, hence the width-1 */
      md = (width-1)/(resolution*msInchesPerUnit(units,centerLat));
      dfDelta = md * scale;
      break;

    default:
      break;
  }

  return dfDelta;
}

/************************************************************************/
/*    static double Pix2Georef(int nPixPos, int nPixMin, double nPixMax,*/
/*                              double dfGeoMin, double dfGeoMax,       */
/*                              bool bULisYOrig)                        */
/*                                                                      */
/*      Utility function to convert a pixel pos to georef pos. If       */
/*      bULisYOrig parameter is set to true then the upper left is      */
/*      considered to be the Y origin.                                  */
/*                                                                      */
/************************************************************************/
double Pix2Georef(int nPixPos, int nPixMin, int nPixMax,
                  double dfGeoMin, double dfGeoMax, int bULisYOrig)
{
  double      dfWidthGeo = 0.0;
  int         nWidthPix = 0;
  double      dfPixToGeo = 0.0;
  double      dfPosGeo = 0.0;
  double      dfDeltaGeo = 0.0;
  int         nDeltaPix = 0;

  dfWidthGeo = dfGeoMax - dfGeoMin;
  nWidthPix = nPixMax - nPixMin;

  if (dfWidthGeo > 0.0 && nWidthPix > 0) {
    dfPixToGeo = dfWidthGeo / (double)nWidthPix;

    if (!bULisYOrig)
      nDeltaPix = nPixPos - nPixMin;
    else
      nDeltaPix = nPixMax - nPixPos;

    dfDeltaGeo = nDeltaPix * dfPixToGeo;

    dfPosGeo = dfGeoMin + dfDeltaGeo;
  }
  return (dfPosGeo);
}

/* This function converts a pixel value in geo ref. The return value is in
 * layer units. This function has been added for the purpose of the ticket #1340 */

double Pix2LayerGeoref(mapObj *map, layerObj *layer, int value)
{
  double cellsize = MS_MAX(MS_CELLSIZE(map->extent.minx, map->extent.maxx, map->width,
                                        map->pixeladjustment),
                           MS_CELLSIZE(map->extent.miny, map->extent.maxy, map->height, 
                                        map->pixeladjustment));

  double resolutionFactor = map->resolution/map->defresolution;
  double unitsFactor = 1;

  if (layer->sizeunits != MS_PIXELS)
    unitsFactor = msInchesPerUnit(map->units,0)/msInchesPerUnit(layer->sizeunits,0);

  return value*cellsize*resolutionFactor*unitsFactor;
}
