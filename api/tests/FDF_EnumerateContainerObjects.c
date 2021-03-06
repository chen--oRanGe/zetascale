//----------------------------------------------------------------------------
// ZetaScale
// Copyright (c) 2016, SanDisk Corp. and/or all its affiliates.
//
// This program is free software; you can redistribute it and/or modify it under
// the terms of the GNU Lesser General Public License version 2.1 as published by the Free
// Software Foundation;
//
// This program is distributed in the hope that it will be useful, but WITHOUT
// ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
// FOR A PARTICULAR PURPOSE. See the GNU Lesser General Public License v2.1 for more details.
//
// A copy of the GNU Lesser General Public License v2.1 is provided with this package and
// can also be found at: http://opensource.org/licenses/LGPL-2.1
// You should have received a copy of the GNU Lesser General Public License along with
// this program; if not, write to the Free Software Foundation, Inc., 59 Temple
// Place, Suite 330, Boston, MA 02111-1307 USA.
//----------------------------------------------------------------------------

/*********************************************
**********   Author:  Lisa

**********   Function: ZSEnumerateContainerObjects
***********************************************/
#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <unistd.h>
#include "zs.h"
static struct ZS_state     *zs_state;
struct ZS_thread_state     *_zs_thd_state;
ZS_container_props_t       p;
ZS_cguid_t                 cguid;
FILE                        *fp;
int                         testCount = 0;

int preEnvironment()
{
  /*  ZS_config_t            fdf.config;

    fdf.config.version                      = 1;
    fdf.config.n_flash_devices              = 1;
    fdf.config.flash_base_name              = "/schooner/data/schooner%d";
    fdf.config.flash_size_per_device_gb     = 12;
    fdf.config.dram_cache_size_gb           = 8;
    fdf.config.n_cache_partitions           = 100;
    fdf.config.reformat                     = 1;
    fdf.config.max_object_size              = 1048576;
    fdf.config.max_background_flushes       = 8;
    fdf.config.background_flush_msec        = 1000;
    fdf.config.max_outstanding_writes       = 32;
    fdf.config.cache_modified_fraction      = 1.0;
    fdf.config.max_flushes_per_mod_check    = 32;
*/
    //ZSLoadConfigDefaults(&zs_state);
    //if(ZSInit( &zs_state, &fdf.config ) != ZS_SUCCESS ) {
    if(ZSInit( &zs_state) != ZS_SUCCESS ) {
         fprintf( fp, "ZS initialization failed!\n" );
         return 0 ;
    }

    fprintf( fp, "ZS was initialized successfully!\n" );

    if(ZS_SUCCESS != ZSInitPerThreadState( zs_state, &_zs_thd_state ) ) {
         fprintf( fp, "ZS thread initialization failed!\n" );
         return 0;
    }
    fprintf( fp, "ZS thread was initialized successfully!\n" );

    /*
    p.durability_level = 0;
    p.fifo_mode = 0;
    p.size_kb = 1024*1024;
    p.num_shards = 1;
    p.persistent = 1;
    p.writethru = ZS_TRUE;
    p.evicting = 0;
    p.async_writes = ZS_TRUE;    
    */
    (void)ZSLoadCntrPropDefaults(&p);
    return 1;
}

void CleanEnvironment()
{
    ZSReleasePerThreadState(&_zs_thd_state);
    ZSShutdown(zs_state);
}

void SetPropMode(ZS_boolean_t evicting,ZS_boolean_t persistent,ZS_boolean_t fifo,ZS_boolean_t writethru,ZS_boolean_t async_writes,ZS_durability_level_t durability)
{
    p.evicting = evicting;
    p.persistent = persistent;
    p.fifo_mode = fifo;
    p.writethru = writethru;
    p.async_writes = async_writes;
    p.durability_level = durability;
}

ZS_status_t OpenContainer(char *cname,uint32_t flags,ZS_cguid_t *cguid)
{
    ZS_status_t           ret;
    ret = ZSOpenContainer(_zs_thd_state,cname,&p,flags, cguid);
    if(ZS_SUCCESS == ret){
        fprintf(fp,"ZSOpenContainer cguid=%ld,cname=%s,mode=%d success\n",*cguid,cname,flags);
    }
    else fprintf(fp, "ZSOpenContainer cguid=%ld,cname=%s,mode=%d fail:%s\n",*cguid,cname,flags,ZSStrError(ret));
    return ret;
}


ZS_status_t CloseContainer(ZS_cguid_t cguid)
{
    ZS_status_t           ret;
    ret = ZSCloseContainer(_zs_thd_state, cguid );
    if(ZS_SUCCESS == ret){
        fprintf(fp,"ZSCloseContainer cguid=%ld success.\n",cguid);
    }
    else fprintf(fp,"ZSCloseContainer cguid=%ld failed:%s.\n",cguid,ZSStrError(ret));
    return ret;
}

ZS_status_t DeleteContainer(ZS_cguid_t cguid)
{ 
    ZS_status_t           ret;
    ret = ZSDeleteContainer (_zs_thd_state, cguid);
    if(ZS_SUCCESS == ret){
        fprintf(fp,"ZSDeleteContainer cguid=%ld success.\n",cguid);
    }
    else fprintf(fp,"DeleteContainer cguid=%ld failed:%s.\n",cguid,ZSStrError(ret));
    return ret;
}

ZS_status_t CreateObject(ZS_cguid_t cguid,char *key,uint32_t keylen,char *data,uint64_t dataln)
{
    ZS_status_t           ret;
    ret = ZSWriteObject(_zs_thd_state,cguid,key,keylen,data,dataln,1);
    if(ZS_SUCCESS == ret){
        fprintf(fp,"ZSWriteObject cguid=%ld,key=%s,data=%s success.\n",cguid,key,data);
    }
    else fprintf(fp,"ZSWriteObject cguid=%ld,key=%s,data=%s failed:%s.\n",cguid,key,data,ZSStrError(ret));
    //sleep(5);
    return ret;
}

ZS_status_t DeleteObject(ZS_cguid_t cguid,char *key,uint32_t keylen)
{
    ZS_status_t           ret;
    ret = ZSDeleteObject(_zs_thd_state,cguid,key,keylen);
    if(ZS_SUCCESS == ret){
        fprintf(fp,"ZSDeleteObject cguid=%ld,key=%s success.\n",cguid,key);
    }
    else fprintf(fp,"ZSDeleteObject cguid=%ld,key=%s failed:%s.\n",cguid,key,ZSStrError(ret));
    return ret;
}

ZS_status_t EnumerateContainerObjects(ZS_cguid_t cguid,struct ZS_iterator **iterator)
{
    ZS_status_t           ret;
    ret = ZSEnumerateContainerObjects(_zs_thd_state,cguid,iterator);
    if(ZS_SUCCESS == ret){
        fprintf(fp,"ZSEnumerateContainerObjects cguid=%ld return success.\n",cguid);
    }
    else fprintf(fp,"ZSEnumerateContainerObjects cguid=%ld return fail:%s.\n",cguid,ZSStrError(ret));
    return ret;
}


ZS_status_t NextEnumeratedObject(struct ZS_iterator *iterator)
{
    char                   *key;
    uint32_t               keylen;
    char                   *data;
    uint64_t               datalen;
    ZS_status_t           ret;

    ret = ZSNextEnumeratedObject(_zs_thd_state,iterator,&key,&keylen,&data,&datalen);
    if(ZS_SUCCESS == ret){
        fprintf(fp,"ZSNextEnumeratedObject return success.\n");
        fprintf(fp,"Object:key=%s,keylen=%d,data=%s,datalen=%ld.\n",key,keylen,data,datalen);
    }
    else fprintf(fp,"ZSNextEnumeratedObject return fail:%s.\n",ZSStrError(ret));
    return ret;
}

ZS_status_t FinishEnumeration(struct ZS_iterator *iterator)
{
    ZS_status_t           ret;

    ret = ZSFinishEnumeration(_zs_thd_state,iterator);
    if(ZS_SUCCESS == ret){
        fprintf(fp,"ZSFinishEnumeration return success.\n");
    }
    else fprintf(fp,"ZSFinishEnumeration return fail:%s.\n",ZSStrError(ret));
    return ret;
}


int ZSEnumerateContainerObjects_basic_check1()
{

    ZS_status_t           ret = ZS_SUCCESS;
    int                    flag;
    struct ZS_iterator    *iterator;
    fprintf(fp,"test %d:\n",++testCount);

    ret = CreateObject(cguid,"key",4,"data",5);
    if(ZS_SUCCESS != ret)
        flag =  -1;
    else{
        ret = EnumerateContainerObjects(cguid,&iterator);
        if(ZS_SUCCESS == ret){
            while(ZS_SUCCESS == NextEnumeratedObject(iterator));
            FinishEnumeration(iterator);
            flag = 1;
        }
        else flag = 0;
    }
    if(ZS_SUCCESS != DeleteObject(cguid,"key",4))flag = -3;
    return flag;    
}

int ZSEnumerateContainerObjects_basic_check2()
{
    ZS_status_t           ret = ZS_SUCCESS;
    int                    flag;
    struct ZS_iterator    *iterator;
    fprintf(fp,"test %d:\n",++testCount);

    ret = CreateObject(cguid,"key",4,"data",5);
    if(ZS_SUCCESS != ret)
       flag =  -1;
    else{
       ret = EnumerateContainerObjects(cguid,&iterator);
       if(ZS_SUCCESS == ret){
           //while(ZS_SUCCESS == NextEnumeratedObject(iterator));
           FinishEnumeration(iterator);
           flag = 1;
       }
       else flag = 0;
    }
    if(ZS_SUCCESS != DeleteObject(cguid,"key",4)) flag = -3;
    return flag;
}

int ZSEnumerateContainerObjects_noObject1()
{

    ZS_status_t           ret;
    int                    flag;
    struct ZS_iterator    *iterator;
    fprintf(fp,"test %d:\n",++testCount);

    ret = EnumerateContainerObjects(cguid,&iterator);
    if(ZS_SUCCESS == ret){
        while(ZS_SUCCESS == NextEnumeratedObject(iterator));
        FinishEnumeration(iterator);
        flag = 1;
    }
    else flag = 0;
       
    return flag;
}

int ZSEnumerateContainerObjects_noObject2()
{

    ZS_status_t           ret = ZS_SUCCESS;
    int                    flag;
    struct ZS_iterator    *iterator;
    fprintf(fp,"test %d:\n",++testCount);

    ret = CreateObject(cguid,"key",4,"data",5);
    if(ZS_SUCCESS != ret){
        return -1;
    }    
    DeleteObject(cguid,"key",4);

    ret = EnumerateContainerObjects(cguid,&iterator);
    if(ZS_SUCCESS == ret){
        while(ZS_SUCCESS == NextEnumeratedObject(iterator));
        FinishEnumeration(iterator);
        flag = 1;
    }
    else flag = 0;

    return flag;
}

int ZSEnumerateContainerObjects_Twice()
{

    ZS_status_t           ret = ZS_SUCCESS;
    int                    flag;
    struct ZS_iterator    *iterator1,*iterator2;
    fprintf(fp,"test %d:\n",++testCount);

    ret = CreateObject(cguid,"key",4,"data",5);
    if(ZS_SUCCESS != ret){
        return -1;
    }

    ret = EnumerateContainerObjects(cguid,&iterator1);
    if(ZS_SUCCESS == ret){
        while(ZS_SUCCESS == NextEnumeratedObject(iterator1));

        ret = EnumerateContainerObjects(cguid,&iterator2);
        if(ZS_SUCCESS == ret){
            while(ZS_SUCCESS == NextEnumeratedObject(iterator2));
            FinishEnumeration(iterator2);
            fprintf(fp,"EnumerateContainerObjects contiuous twice success.\n");
            flag = 1;
        }
        else{
            fprintf(fp,"EnumerateContainerObjects contiuous twice fail:%s.\n",ZSStrError(ret));
            flag = 0;
        }

        FinishEnumeration(iterator1);
    }
    else flag = -1;

    if(ZS_SUCCESS != DeleteObject(cguid,"key",4)) flag = -3;   
    return flag;
}

int ZSEnumerateContainerObjects_noObject_close()
{

    ZS_status_t           ret = ZS_SUCCESS;
    int                    flag;
    struct ZS_iterator    *iterator;
    fprintf(fp,"test %d:\n",++testCount);

    CloseContainer(cguid );

    ret = EnumerateContainerObjects(cguid,&iterator);
    if(ZS_SUCCESS == ret){
        while(ZS_SUCCESS == NextEnumeratedObject(iterator));
        FinishEnumeration(iterator);
        flag = 1;
    }
    else flag = 0;

    OpenContainer("x",ZS_CTNR_RW_MODE,&cguid);
    return flag;
}

int ZSEnumerateContainerObjects_invalid_cguid()
{

    ZS_status_t           ret = ZS_SUCCESS;
    int                    flag;
    struct ZS_iterator    *iterator;
    fprintf(fp,"test %d:\n",++testCount);

    ret = CreateObject(cguid,"key",4,"data",5);
    if(ZS_SUCCESS != ret){
        return -1;
    }

    ret = ZSEnumerateContainerObjects(_zs_thd_state,-1,&iterator);
    if(ZS_SUCCESS == ret){
        while(ZS_SUCCESS == NextEnumeratedObject(iterator));
        FinishEnumeration(iterator);
        fprintf(fp,"EnumerateContainerObjects use invalid cguid return success.\n");
        flag = 0;
    }
    else{
        fprintf(fp,"EnumerateContainerObjects use invalid cguid return fail:%s.\n",ZSStrError(ret));
        flag = 1;
    }

    if(ZS_SUCCESS != DeleteObject(cguid,"key",4)) flag = -3;
    return flag;
}

int ZSEnumerateContainerObjects_MoreObject1(int count)
{

    ZS_status_t           ret = ZS_SUCCESS;
    int                    flag;
    struct ZS_iterator    *iterator;
    char                   key[5] = "key1";
    fprintf(fp,"test %d:\n",++testCount);

    for(int i =0; i < count; i++){
        ret = CreateObject(cguid,key,5,"data",5);
        if(ZS_SUCCESS != ret){
            flag = -1;
            for(int j = i-1;j >= 0;j--){
                key[3]--;
                if(ZS_SUCCESS != DeleteObject(cguid,key,5)) flag = -3;
            }

            return flag;
        }

        key[3]++;
    }

    ret = EnumerateContainerObjects(cguid,&iterator);
    if(ZS_SUCCESS == ret){
        while(ZS_SUCCESS == NextEnumeratedObject(iterator));
        FinishEnumeration(iterator);
        flag = 1;
    }
    else flag = 0;

    for(int j = count-1;j >= 0;j--){
        key[3]--;
        if(ZS_SUCCESS != DeleteObject(cguid,key,5))flag = -3;
    }
    return flag;
}

int ZSEnumerateContainerObjects_MoreObject2(int count)
{

    ZS_status_t           ret1 = ZS_SUCCESS,ret2;
    ZS_cguid_t            cguid1;
    int                    flag;
    struct ZS_iterator    *iterator1,*iterator2;
    char                   key1[6] = "key_a";
    char                   key2[7] = "test_1";
    char                   data1[7] = "data_a";
    char                   data2[7] = "data_1";
    fprintf(fp,"test %d:\n",++testCount);

    OpenContainer("test",ZS_CTNR_CREATE,&cguid1);

    for(int i =0; i < count; i++){
        ret1 = CreateObject(cguid,key1,6,data1,7);
        ret2 = CreateObject(cguid1,key2,7,data2,7);
        if(ZS_SUCCESS != ret1 || ZS_SUCCESS != ret2){
            flag = -1;
            if(ZS_SUCCESS == ret1)
                if(ZS_SUCCESS != DeleteObject(cguid,key1,6)) flag = -3;
            if(ZS_SUCCESS == ret2)
                if(ZS_SUCCESS != DeleteObject(cguid1,key2,7)) flag = -3;
            for(int j = i-1;j >= 0;j--){
                key1[4]--;
                if(ZS_SUCCESS != DeleteObject(cguid,key1,6)) flag = -3;
                key2[5]--;
                if(ZS_SUCCESS != DeleteObject(cguid1,key2,7)) flag = -3;
            }
            if(ZS_SUCCESS != CloseContainer(cguid1 ))flag = -3;
            if(ZS_SUCCESS != DeleteContainer(cguid1))flag = -3;
            return flag;
        }

        key1[4]++;
        key2[5]++;
    }

    ret1 = EnumerateContainerObjects(cguid,&iterator1);
    ret2 = EnumerateContainerObjects(cguid1,&iterator2);
    if(ZS_SUCCESS == ret1 && ZS_SUCCESS == ret2){
        while(ZS_SUCCESS == NextEnumeratedObject(iterator1));
        while(ZS_SUCCESS == NextEnumeratedObject(iterator2));
        FinishEnumeration(iterator1);
        FinishEnumeration(iterator2);
        flag = 1;
    }
    else{
        if(ZS_SUCCESS == ret1){
            while(ZS_SUCCESS == NextEnumeratedObject(iterator1));
            FinishEnumeration(iterator1);
        }
        if(ZS_SUCCESS == ret2){
            while(ZS_SUCCESS == NextEnumeratedObject(iterator2));
             FinishEnumeration(iterator2);
        }
            
        flag = 0;
    }

    for(int j = count-1;j >= 0;j--){
        key1[4]--;
        key2[5]--;
        if(ZS_SUCCESS != DeleteObject(cguid,key1,6)) flag = -3;
        if(ZS_SUCCESS != DeleteObject(cguid1,key2,7)) flag = -3;
    }
    if(ZS_SUCCESS != CloseContainer(cguid1 ))flag = -3;
    if(ZS_SUCCESS != DeleteContainer(cguid1))flag = -3;
    return flag;
}

int ZSEnumerateContainerObjects_Open_CreateObj_close1()
{

    ZS_status_t           ret = ZS_SUCCESS;
    int                    flag;
    struct ZS_iterator    *iterator;
    fprintf(fp,"test %d:\n",++testCount);

    ret = CreateObject(cguid,"key1",5,"data",5);
    if(ZS_SUCCESS != ret){
        return -1;
    }
    CloseContainer(cguid );
    OpenContainer("key",ZS_CTNR_RO_MODE,&cguid);

    ret = EnumerateContainerObjects(cguid,&iterator);
    if(ZS_SUCCESS == ret){
        while(ZS_SUCCESS == NextEnumeratedObject(iterator));
        FinishEnumeration(iterator);
        flag = 1;
    }
    else flag = 0;
    
#ifdef RO_MODE_SUPPORTED
    if(ZS_FAILURE != DeleteObject(cguid,"key1",5)) flag = -3; 
#else 
    if(ZS_SUCCESS != DeleteObject(cguid,"key1",5)) flag = -3; 
#endif
    return flag;
}

int ZSEnumerateContainerObjects_Open_CreateObj_close2()
{

    ZS_status_t           ret = ZS_SUCCESS;
    int                    flag;
    struct ZS_iterator    *iterator;
    fprintf(fp,"test %d:\n",++testCount);

    ret = CreateObject(cguid,"key1",5,"data",5);
    if(ZS_SUCCESS != ret){
        return -1;
    }
    CloseContainer(cguid );
    //OpenContainer("key",ZS_CTNR_RW_MODE,&cguid);
    //CreateObject(cguid,"key2",5,"data",5);

    ret = EnumerateContainerObjects(cguid,&iterator);
    if(ZS_SUCCESS == ret){
    while(ZS_SUCCESS == NextEnumeratedObject(iterator));
        FinishEnumeration(iterator);
        flag = 1;
    }
    else flag = 0;
    OpenContainer("key",ZS_CTNR_RW_MODE,&cguid); 
    if(ZS_SUCCESS != DeleteObject(cguid,"key1",5)) flag = -3;
    //DeleteObject(cguid,"key2",5);
    return flag;
}

/**********  main function *******/

int main(int argc, char *argv[])
{
    int result[3][13] = {{0,0}};
    ZS_boolean_t eviction[] = {0,0,0};
    ZS_boolean_t persistent[] = {1,1,1};
    ZS_boolean_t fifo[] = {0,0,0};
    ZS_boolean_t writethru[] = {1,1,1};
    int resultCount = 27;
    int num = 0;
    ZS_boolean_t async_writes[] = {0,1,0};
    ZS_durability_level_t durability[] = {0,1,2};

    if((fp = fopen("ZS_EnumerateContainerObjects.log", "w+")) == 0){
        fprintf(stderr, " open failed!.\n");
        return -1;
    }

    if( 1 != preEnvironment())
        return 0;

    fprintf(fp, "************Begin to test ***************\n");
 
    for(int i = 0 ;  i < 3;i++){
        SetPropMode(eviction[i],persistent[i],fifo[i],writethru[i],async_writes[i],durability[i]);
        testCount = 0;
        OpenContainer("key",ZS_CTNR_CREATE,&cguid);

        result[i][0] = ZSEnumerateContainerObjects_basic_check1();
        result[i][1] = ZSEnumerateContainerObjects_basic_check2();
        result[i][2] = ZSEnumerateContainerObjects_noObject1();
        result[i][3] = ZSEnumerateContainerObjects_noObject2();
        result[i][4] = ZSEnumerateContainerObjects_Twice();
        result[i][5] = ZSEnumerateContainerObjects_invalid_cguid();
        result[i][6] = ZSEnumerateContainerObjects_MoreObject1(2);
        result[i][7] = ZSEnumerateContainerObjects_MoreObject2(3);
        result[i][8] = ZSEnumerateContainerObjects_Open_CreateObj_close1();
        //result[i][9] = ZSEnumerateContainerObjects_Open_CreateObj_close2();
    
        CloseContainer(cguid );
        DeleteContainer(cguid);
    }

    CleanEnvironment();
    
    for(int j = 0; j < 3;j++){
        fprintf(stderr, "test mode:eviction=%d,persistent=%d,fifo=%d,async_writes=%d,durability=%d.\n",eviction[j],persistent[j],fifo[j],async_writes[j],(j+1)%2+1);
        for(int i = 0; i < 9; i++){
            if(result[j][i] == 1){
                num++;
                fprintf(stderr, "ZSEnumerateContainerObjects test %drd success.\n",i+1);
            }
            else if(result[j][i] == -1)
                fprintf(stderr, "ZSEnumerateContainerObjects test %drd fail to test.\n",i+1);
            else if(result[j][i] == 0)
                fprintf(stderr, "ZSEnumerateContainerObjects test %drd failed.\n",i+1);
            else fprintf(stderr, "ZSEnumerateContainerObjects test %drd hit wrong.\n",i+1);
        }
    }

    if(resultCount == num){
        fprintf(stderr, "************ test pass!******************\n");
	fprintf(stderr, "#The related test script is ZS_EnumerateContainerObjects.c\n");
	fprintf(stderr, "#If you want, you can check test details in ZS_EnumerateContainerObjects.log\n");
        return 0;
    }
    else 
        fprintf(stderr, "************%d test fail!******************\n",resultCount-num);
	fprintf(stderr, "#The related test script is ZS_EnumerateContainerObjects.c\n");
	fprintf(stderr, "#If you want, you can check test details in ZS_EnumerateContainerObjects.log\n");
    return 1;
}



