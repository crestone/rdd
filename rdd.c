#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>

#include "hiredis.h"

#define u32 uint32_t
#define u16 uint16_t
#define u8 uint8_t

/*
 
.rdd file format

--- header ---

magic 0x42424242			4
u32 nbkey				4

--- each keys ---

magic 0x4242				2
u16 type				2
u32 full size				4

key name, 0 filled to be aligned	?
u32 nbresult				4
data sizes, u32*(nbresult)		4*nb
each data, 0 filled to be aligned	?

*/

struct keynfo
{	u16 *type;
	char*name;
	u32 *nb;
	u32 *size;
	u32 *sizes;
	char**data;
};

u32 rddAddKeys(char**rdd, u32 nbk, char**kkeys);
u32 rddHasKey(char*rdd,char*name);
char * keyCreate(char*name,u16 type, u32 nb, u32*dsize, char**data);
u32 keyGetSize(char*k);
char * rddGoToKey(char*rdd, u32 id);
char ** rddGetAllKeys(char*rdd, u32*nb);
void rddMerge(char**rdd,char*add);
char * rddnew(void);
u32 rddGetSize(char*rdd);
char ** rddGetKeys(char*rdd, char**filter, u32 nbfilter, u32 method, u32*nb);
char * rddRedis(redisContext*redis,char*filter);
u32 rddMatch(char**rdd, char**filter, u32 nbfilter);
u32 rddFilter(char**rdd, char**filter, u32 nbfilter);
void rddSave(char*rdd, char*fname);
char * rddLoad(char*fname);
u32 getType(char*t);
const char * getTypeName(u16*type);
u32 wildMatch(char* pat, char* str);
u32 keyGetNfo(char*k,struct keynfo *nfo);
void rddRedisConnect(redisContext ** rd, char*server, u32 port, u32 db, char * pass);
void rddPrint(char*rdd,u32 verbose);
void keyPrint(char*key);
u32 rddRedisInsert(redisContext*rd,char*rdd);
void rddRedisDelete(redisContext*rd,char*rdd);
char * rddRedis(redisContext*rd,char*filter);

static const ptrdiff_t kalign = 4;

u32 getType(char*t) // string, list, set, zset and hash
{	switch(*t)
	{	case 'l' : return 1; break;
		case 'z' : return 3; break;
		case 'h' : return 4; break;
		case 's' :
			if(t[1] == 't') return 0; return 2;
		break;
		default : return 255; break;
	}; return 255;
}

const char * getTypeName(u16*type) // string, list, set, zset and hash
{	switch(*type)
	{	case 0: return "string"; break;
		case 1: return "list"; break;
		case 2: return "set"; break;
		case 3: return "zset"; break;
		case 4: return "hash"; break;
		default: return "unknow"; break;
	}; return ":)";
}


//wilcard function come from here, http://xoomer.virgilio.it/acantato/dev/wildcard/wildmatch.html
u32 wildMatch(char* pat, char* str) {
   char* s, *p;
   u32 star = 0;

loopStart:
   for (s = str, p = pat; *s; ++s, ++p) {
      switch (*p) {
         case '?':
            if (*s == '.') goto starCheck;
            break;
         case '*':
            star = 1;
            str = s, pat = p;
            do { ++pat; } while (*pat == '*');
            if (!*pat) return 1;
            goto loopStart;
         default:
            if (*s != *p) goto starCheck;
            break;
      } /* endswitch */
   } /* endfor */
   while (*p == '*') ++p;
   return (!*p);

starCheck:
   if (!star) return 0;
   str++;
   goto loopStart;
}

u32 rddHasKey(char*rdd,char*name)
{	u32 *p = (u32*)rdd; if(*p++ != 0x42424242) return 0; u32 nb = *p++;
	for(u32 c=0;c<nb;c++)
	{	char *k = (char*)p; k+=8;
		if(!strcmp(k,name)) return c;
		p++; p += ((*p)-4)>>2;	
	}
	return 0xffffffff;
}

char * keyCreate(char*name,u16 type, u32 nb, u32*dsize, char**data)
{	u32 size = 12+nb*4; // magic,type,size,nbres,data size
	u32*sizes = (u32*)malloc((nb+1)*4);
	u32 tmp = strlen(name)+1; tmp = (tmp + (kalign - 1)) & -kalign;
	
	*sizes = tmp;
	size += tmp; // strlen, 4o aligned
	
	for(u32 c=0;c<nb;c++)
	{	tmp = dsize[c]; tmp = (tmp + (kalign - 1)) & -kalign;
		size += tmp;
		sizes[c+1] = tmp;
	}
	
	char *k = (char*)calloc(1,size);
	char *p = k;
	*(u16*)p = 0x4242;
	*(u16*)(p+2) = type;
	*(u32*)(p+4) = size;
	strcpy(p+8,name);
	p += 8+(*sizes);
	u32 *p32 = (u32*)p;
	*p32++ = nb;
	for(u32 c=0;c<nb;c++) *p32++ = dsize[c];
	p = (char*)p32;
	for(u32 c=0;c<nb;c++)
	{	memcpy(p,data[c],dsize[c]);
		p += sizes[c+1];
	}
	free(sizes);
	return k;
}

u32 keyGetSize(char*k)
{	if(*(u16*)k != 0x4242) return 0;
	k+=4; return *(u32*)k;
}

u32 keyGetNfo(char*k,struct keynfo *nfo)
{	if(*(u16*)k != 0x4242) return 0;
	nfo->type = (u16*)(k+2);
	nfo->size = (u32*)(k+4);
	nfo->name = k+8;
	nfo->nb = (u32*)k;
	u32 tmp = strlen(nfo->name)+1; tmp = (tmp + (kalign - 1)) & -kalign;
	nfo->nb = (u32*)(k+8+tmp);
	nfo->sizes = &(nfo->nb[1]);
	nfo->data = (char**)malloc((*nfo->nb)*sizeof(char*));
	char*nfodata = (char*)(nfo->sizes); nfodata += 4*(*nfo->nb);
	for(u32 c=0;c < *nfo->nb;c++)
	{	(nfo->data)[c] = nfodata;
		u32 size = nfo->sizes[c];
		size = (size + (kalign - 1)) & -kalign;
		nfodata += size;
	}	return 1;
}

char * rddGoToKey(char*rdd, u32 id)
{	u32  *p = (u32*)rdd; if(*p++ != 0x42424242) return 0;
	if(id >= *p++) return 0;
	for(u32 c=0;c<id;c++){ p++; p += ((*p)-4)>>2; }
	return (char*)p;
}

char ** rddGetAllKeys(char*rdd, u32*nb)
{	u32  *p = (u32*)rdd; if(*p++ != 0x42424242) { *nb=0; return 0; } *nb=*p++;
	char**out = (char**)malloc((*nb)*sizeof(char*));
	for(u32 c=0;c<*nb;c++)
	{	out[c] = (char*)p++; p += ((*p)-4)>>2;	
	}
	return out;
}

char ** rddGetKeys(char*rdd, char**filter, u32 nbfilter, u32 method, u32*nb)
{	u32  *p = (u32*)rdd; if(*p++ != 0x42424242) { *nb=0; return 0; }
	u32 nbkey;
	char**allk = rddGetAllKeys(rdd,&nbkey);
	char**validk = (char**)malloc(nbkey*sizeof(char*));
	u32 match;
	*nb=0;
	
	for(u32 c=0;c<nbkey;c++)
	{	char *k = allk[c];
		char *n = k+8;
		match = 0;
		for(u32 f=0;f<nbfilter;f++) if(wildMatch(filter[f],n)) match++;
		if( ( (match == nbfilter) && method ) || ( !match && !method ) ) { validk[*nb] = k; *nb = (*nb)+1; }
	}

	free(allk);
	return validk;
}

u32 rddFilter(char**rdd, char**filters, u32 nbfilters)
{	if(*(u32*)*rdd != 0x42424242) return 0;
	u32 nbkey;
	char ** keys = rddGetKeys(*rdd,filters,nbfilters,0, &nbkey);
	char *out = rddnew();	
	rddAddKeys(&out,nbkey,keys);
	free(*rdd); *rdd=out;	
	return 1;
}

u32 rddMatch(char**rdd, char**filters, u32 nbfilters)
{	if(*(u32*)*rdd != 0x42424242) return 0;
	u32 nbkey;
	char ** keys = rddGetKeys(*rdd,filters,nbfilters,1, &nbkey);
	char *out = rddnew();	
	rddAddKeys(&out,nbkey,keys);
	free(*rdd); *rdd=out;	
	return 1;
}

void rddMerge(char**rdd,char*add) // merge add into rdd
{	u32 addnb = 0;
	char**k = rddGetAllKeys(add,&addnb);
	rddAddKeys(rdd,addnb,k); free(k);
}

char * rddRedis(redisContext*rd,char*filter)
{	const char * const redisGetCmd[5] = { "GET %s", "LRANGE %s 0 -1", "SMEMBERS %s", "ZRANGE %s 0 -1 WITHSCORES", "HGETALL %s" };

	redisReply *keys = redisCommand(rd,"KEYS %s",filter);
	if(keys->type != REDIS_REPLY_ARRAY) return 0;

	char * rdd = rddnew();
	u32 nbkey = keys->elements; if(!nbkey) { freeReplyObject(keys); return rdd; }

	u8 *ktype = (u8*)malloc(nbkey);
	char **rddkeys = (char**)malloc(sizeof(char*)*nbkey);
	
	redisReply *reply;
	
	for(u32 n=0;n<nbkey;n++) redisAppendCommand(rd,"TYPE %s",keys->element[n]->str); // get key type
	for(u32 n=0;n<nbkey;n++)
	{	redisGetReply(rd,(void**)&reply);
		u8 t = getType(reply->str); ktype[n] = t;
		redisAppendCommand(rd,redisGetCmd[t],keys->element[n]->str); // get data
		freeReplyObject(reply);
	}

	for(u32 n=0;n<nbkey;n++)
	{	redisGetReply(rd,(void**)&reply);
		if(reply->type == REDIS_REPLY_STRING)
		{	u32 datasize = reply->len;
			rddkeys[n] = keyCreate(keys->element[n]->str,ktype[n],1,&datasize,&reply->str);
		}
	       else
		if(reply->type == REDIS_REPLY_ARRAY)
		{	u32 nbres = reply->elements;
			char ** data = (char**)malloc(nbres*sizeof(char*));
			u32 *datasize = (u32*)malloc(nbres*4);
			
			for(u32 c=0;c<nbres;c++)
			{	datasize[c] = reply->element[c]->len;
				data[c] = reply->element[c]->str;
			}
			  
			rddkeys[n] = keyCreate(keys->element[n]->str,ktype[n],nbres,datasize,data);
			free(data); free(datasize);
		}
		freeReplyObject(reply);
	}
	
	freeReplyObject(keys);
	
	rddAddKeys(&rdd, nbkey, rddkeys);
	return rdd;
}

void rddRedisDelete(redisContext*rd,char*rdd)
{	u32 nb=0; char **keys = rddGetAllKeys(rdd,&nb);
	int argnb = nb+1;
	const char ** cargv = (const char**) malloc( argnb*(sizeof(char*)) );
	size_t *cargl = (size_t*)malloc( argnb*(sizeof(size_t*)) );
		
	cargv[0] = "DEL";
	cargl[0] = 3;
		
	for(u32 c=0;c<nb;c++)
	{	char*n = (keys[c])+8;
		cargv[c+1] = n;
		cargl[c+1] = strlen(n);
	}
	redisReply * reply = redisCommandArgv(rd, argnb, cargv, cargl);
	
	//printf("%i keys where removed from redis.\n",reply->integer);
	
	free(cargv);
	free(cargl);	
	free(keys);
}


u32 rddRedisInsert(redisContext*rd,char*rdd)
{	rddRedisDelete(rd,rdd);
	const char * const redisSetCmd[5] = { "SET", "RPUSH", "SADD", "ZADD", "HMSET" };
	u32 nb=0; char **keys = rddGetAllKeys(rdd,&nb);
	
	for(u32 c=0;c<nb;c++)
	{	struct keynfo k; 
		keyGetNfo(keys[c],&k);

		int argnb = (*k.nb) + 2;
		
		const char ** cargv = (const char**) malloc( argnb*(sizeof(char*)) );
		size_t *cargl = (size_t*)malloc( argnb*(sizeof(size_t*)) );
		
		cargv[0] = redisSetCmd[*k.type];
		cargl[0] = strlen(cargv[0]);
		
		cargv[1] = (const char *)k.name;
		cargl[1] = strlen(cargv[1]);

		if(*k.type == 3)	// zset, need invert args...
		{	for(u32 n=0;n<(*(k.nb));n+=2)
			{	cargv[n+2] = (const char *)(k.data[n+1]); // score
				cargl[n+2] = k.sizes[n+1];
				cargv[n+3] = (const char *)(k.data[n]); // data
				cargl[n+3] = k.sizes[n];
			}
		} else {
			
			for(u32 n=0;n<(*(k.nb));n++)
			{	cargv[n+2] = (const char *)(k.data[n]);
				cargl[n+2] = k.sizes[n];
			}
		}
		
		redisAppendCommandArgv(rd, argnb, cargv, cargl);
		
		free(k.data);
		free(cargv);
		free(cargl);
	}
	
	free(keys);
	
	redisReply * reply;
	for(u32 c=0;c<nb;c++)
	{	redisGetReply(rd,(void**)&reply);
		freeReplyObject(reply);
	}
	return 1;
}
	
char * rddLoad(char*fname)
{	FILE *f = fopen(fname,"r"); fseek(f,0,SEEK_END); u32 fsize = ftell(f); fseek(f,0,SEEK_SET);
	void *data = malloc(fsize);
	if(fread(data,fsize,1,f)!=1) return 0;
	fclose(f); return (char*)data;
}

char * rddnew(void)
{	u32 *rdd = (u32*)malloc(8);
	*rdd = 0x42424242; rdd[1]=0;
	return (char*)rdd;
}

u32 rddGetSize(char*rdd)
{	u32  *p = (u32*)rdd; if(*p++ != 0x42424242) return 0;
	u32 size=8;
	u32 nbkey = *p++;
	char *cp = (char*)p;
	for(u32 c=0;c<nbkey;c++)
	{	u32 sz = *++p;
		cp+=sz; p = (u32*)cp;
		size+=sz;
	}
	return size;
}

void rddSave(char*rdd, char*fname)
{	FILE * f = fopen(fname,"wb");
	fwrite(rdd,1,rddGetSize(rdd),f);
	fclose(f); 
}

u32 rddAddKeys(char**rdd, u32 nbk, char**kkeys)
{	if(*(u32*)*rdd != 0x42424242) return 0;
	u32 size = rddGetSize(*rdd);
	u32 nsize = size;
	u32*ksize = (u32*)malloc(nbk*4);
	char**keys = (char**)malloc(nbk*sizeof(char*));
	u32 nb = 0;
	
	for(u32 c=0;c<nbk;c++)
	{	if(0xffffffff == rddHasKey(*rdd,kkeys[c]+8)) keys[nb++] = kkeys[c];
	}
	
	for(u32 c=0;c<nb;c++)
	{	u32 tmp = keyGetSize(keys[c]);
		ksize[c] = tmp; nsize += tmp;
	}
	
	*rdd = (char*)realloc((void*)*rdd,nsize);
	*(u32*)((*rdd)+4) += nb; // write new rddkey nb
	char *p = *rdd; p += size;
	for(u32 c=0;c<nb;c++)
	{	memcpy(p,keys[c],ksize[c]);
		p+=ksize[c];
	}
	free(ksize);
	free(keys);
	return 1;
}

void keyPrint(char*key)
{	struct keynfo k;
	keyGetNfo(key,&k);
	char * header = "------------------";
	printf("%s\n%s, %u elements\n",header,getTypeName(k.type),*k.nb);
	for(u32 c=0;c<*k.nb;c++)
	{	if(k.sizes[c])
		{	char *p = &(k.data[c][k.sizes[c]]); p--; char save = *p; *p = 0;
			printf("%2u %u\t[%*s%c]\n",c,k.sizes[c],k.sizes[c]-1,k.data[c],save); *p = save;
		} else printf("%2u %u\t[]\n",c,k.sizes[c]);
	}
	printf("%s\n",header);
	free(k.data);
}

void rddPrint(char*rdd,u32 verbose)
{	if(*(u32*)rdd != 0x42424242) { printf("error, not a rdd\n"); return; }
	u32 nbkey; char ** keys = rddGetAllKeys(rdd,&nbkey);
	
	u32 digit=1, limit=10; while(nbkey > limit){ limit*=10; digit++; }
	
	char * header = "----------------------------------------------------";
	printf("%s\n---- %u keys (%uo)\n%s\n",header,nbkey,rddGetSize(rdd),header);
	for(u32 c=0;c<nbkey;c++)
	{	printf("-%*u- %s\n",digit,c,keys[c]+8);
		if(verbose>1) keyPrint(keys[c]);
	}
	free(keys);
	printf("%s\n",header);
}

void rddRedisConnect(redisContext ** rd, char*server, u32 port, u32 db, char * pass)
{	struct timeval timeout = { 1, 500000 };
	*rd = redisConnectWithTimeout(server,port,timeout);
	if ((*rd)->err) {
	    printf("\nredis connection error: %s\n", (*rd)->errstr);
	    exit(1);
	}

	if(pass)
	{	redisReply *reply = redisCommand(*rd,"AUTH %s",pass);
		if(strcmp(reply->str,"OK"))
		{	printf("\nredis password error\n");
			exit(1);
		}	free(reply);
	}
	
	if(db)
	{	redisReply *reply = redisCommand(*rd,"SELECT %u",db);
		if(strcmp(reply->str,"OK"))
		{	printf("\nredis select db (%u) error\n",db);
			exit(1);
		}	free(reply);
	}
}

int main(int argc,char **argv)
{	char redisserver[1024] = "127.0.0.1\0";
	u32  redisport = 6379;
	u32  redisdb = 0;
	char *redisauth = 0;
	char *input[255];	// rdd/redis inputs
	u32 inputnb = 0;
	char *filter[255];	// filter to trim from output
	u32 filternb = 0;
	char *match[255];	// filter to keep from output
	u32 matchnb = 0;
	char *out = 0;
	char verbose=0;
	
	redisContext *redis = 0;
	
	int i=1, filterflag=0; // 0:input, 1:filter, 2:match
	while(i < argc)
	{	if(!strcmp(argv[i],"-h"))
		{	printf("rdd'r 0.1\n\nusage:\t-h : show this help screen\n"); return 0;
		}
		
		if(!strcmp(argv[i],"-s"))
		{	if(argc < i+2) return 1;
			char*server=argv[++i]; if(strlen(redisserver) > 1024) return 1; strcpy(redisserver,server); i++; continue;
		}
		
		if(!strcmp(argv[i],"-p"))
		{	if(argc < i+2) return 1;
			redisport = (u32)atoi(argv[++i]);
			i++; continue;
		}
		
		if(!strcmp(argv[i],"-d"))
		{	if(argc < i+2) return 1;
			redisdb = (u32)atoi(argv[++i]);
			i++; continue;
		}		
		
		if(!strcmp(argv[i],"-a"))
		{	if(argc < i+2) return 1;
			redisauth=argv[++i];
			i++; continue;
		}
		
		if(!strcmp(argv[i],"-o"))
		{	if(argc < i+2) return 1;
			out=argv[++i];
			i++; continue;
		}
		
		if(!strcmp(argv[i],"-v")){ i++; verbose++; continue; }
		
		if(!strcmp(argv[i],"-i")){filterflag=0; i++; continue;}
		if(!strcmp(argv[i],"-f")){filterflag=1; i++; continue;}
		if(!strcmp(argv[i],"-m")){filterflag=2; i++; continue;}
		
		if(!filterflag)   { input[inputnb++] = argv[i++]; continue; }
		if(filterflag==1) { filter[filternb++] = argv[i++]; continue; }
		if(filterflag==2) { match[matchnb++] = argv[i++]; continue; }
	}

	if(inputnb == 0) // no input mean all redis keys
	{	input[inputnb++] = (char*)"*";
	}
	
	char *inputrdd = rddnew();
	
	for(u32 cpt=0;cpt<inputnb;cpt++)
	{	char *rdd;
		if(NULL == strstr(input[cpt],".rdd")) // redis;
		{	if(!redis) rddRedisConnect(&redis,redisserver,redisport,redisdb,redisauth);
			rdd = rddRedis(redis,input[cpt]);
		}
		else
		{	rdd = rddLoad(input[cpt]);
			if(!rdd) { printf("rdd load error on [%s]",input[cpt]); exit(1); }
		}
		rddMerge(&inputrdd,rdd); free(rdd);
	}

	if(filternb) rddFilter(&inputrdd,filter,filternb);
	if(matchnb)  rddMatch(&inputrdd,match,matchnb);
	
	if(out)
	{	if(!strcmp(out,"insert")) // insert keys in redis
		{	if(!redis) rddRedisConnect(&redis,redisserver,redisport,redisdb,redisauth);
			rddRedisInsert(redis,inputrdd);
		}
		else
		if(!strcmp(out,"delete")) // delete keys in redis
		{	if(!redis) rddRedisConnect(&redis,redisserver,redisport,redisdb,redisauth);
			rddRedisDelete(redis,inputrdd);
		}
		else
		{	if(NULL != strstr(out,".rdd")) rddSave(inputrdd,out); else verbose++;
		}
	} else verbose++;
	
	if(verbose) rddPrint(inputrdd,verbose);
	
	free(inputrdd); if(redis) redisFree(redis);
	return 0;
}