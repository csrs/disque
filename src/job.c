/* Jobs handling and commands.
 *
 * Copyright (c) 2014, Salvatore Sanfilippo <antirez at gmail dot com>
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 *   * Redistributions of source code must retain the above copyright notice,
 *     this list of conditions and the following disclaimer.
 *   * Redistributions in binary form must reproduce the above copyright
 *     notice, this list of conditions and the following disclaimer in the
 *     documentation and/or other materials provided with the distribution.
 *   * Neither the name of Disque nor the names of its contributors may be used
 *     to endorse or promote products derived from this software without
 *     specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#include "disque.h"
#include "cluster.h"
#include "job.h"
#include "queue.h"
#include "sha1.h"
#include "endianconv.h"

/* ------------------------- Low level jobs functions ----------------------- */

/* Generate a new Job ID and writes it to the string pointed by 'id'
 * (NOT including a null term), that must be JOB_ID_LEN or more.
 *
 * An ID is composed as such:
 *
 * +--+--------------------------+------------------------------+----+--+
 * |DI| Node ID prefix (8 bytes) | 128-bit rand (hex: 32 bytes) |TTL |SQ|
 * +--+--------------------------+------------------------------+----+--+
 *
 * "DI" is just a fixed string. All Disque job IDs start with this
 * two bytes.
 *
 * Node ID is the first 8 bytes of the hexadecimal Node ID where the
 * message was created. The main use for this is that a consumer receiving
 * messages from a given queue can collect stats about where the producers
 * are connected, and switch to improve the cluster efficiency.
 *
 * 128 bit rand (in hex format) is 32 random chars.
 *
 * The TTL is a big endian 16 bit unsigned number ceiled to 2^16-1
 * if greater than that, and is only used in order to expire ACKs
 * when the job is no longer avaialbe. It represents the TTL of the
 * original job in *minutes*, not seconds, and is encoded in as a
 * 4 digits hexadecimal number.
 *
 * "SQ" is just a fixed string. All Disque job IDs end with this two bytes.
 */
void generateJobID(char *id, int ttl) {
    char *charset = "0123456789abcdef";
    SHA1_CTX ctx;
    unsigned char hash[22]; /* 16 + 2 bytes for TTL. */
    int j;
    static uint64_t counter;

    /* Get the pseudo random bytes using SHA1 in counter mode. */
    counter++;
    SHA1Init(&ctx);
    SHA1Update(&ctx,(unsigned char*)server.jobid_seed,DISQUE_RUN_ID_SIZE);
    SHA1Update(&ctx,(unsigned char*)&counter,sizeof(counter));
    SHA1Final(hash,&ctx);

    ttl /= 60; /* Store TTL in minutes. */
    hash[16] = (ttl&0xff00)>>8;
    hash[17] = ttl&0xff;

    *id++ = 'D';
    *id++ = 'I';

    /* 8 bytes from Node ID */
    for (j = 0; j < 8; j++) *id++ = server.cluster->myself->name[j];

    /* Convert 18 bytes (16 pseudorandom + 2 TTL in minutes) to hex. */
    for (j = 0; j < 18; j++) {
        id[0] = charset[(hash[j]&0xf0)>>4];
        id[1] = charset[hash[j]&0xf];
        id += 2;
    }

    *id++ = 'S';
    *id++ = 'Q';
}

/* Create a new job in a given state. If "ID" is NULL, a new ID will be
 * created as assigned.
 *
 * This function only creates the job without any body, the only populated
 * fields are the ID and the state. */
job *createJob(char *id, int state, int ttl) {
    job *j = zmalloc(sizeof(job));

    /* Generate a new Job ID if not specified by the caller. */
    if (id == NULL)
        generateJobID(j->id,ttl);
    else
        memcpy(j->id,id,JOB_ID_LEN);

    j->queue = NULL;
    j->state = state;
    j->flags = 0;
    j->body = NULL;
    j->nodes_delivered = NULL;
    j->nodes_confirmed = NULL;
    return j;
}

/* Free a job. Does not automatically unregister it. */
void freeJob(job *j) {
    decrRefCount(j->queue);
    sdsfree(j->body);
    if (j->nodes_delivered) dictRelease(j->nodes_delivered);
    if (j->nodes_confirmed) dictRelease(j->nodes_confirmed);
    zfree(j);
}

/* Add the job in the jobs hash table, so that we can use lookupJob()
 * (by job ID) later. If a node knows about a job, the job must be registered
 * and can be retrieved via lookupJob(), regardless of is state.
 *
 * On success DISQUE_OK is returned. If there is already a node with the
 * specified ID, no operation is performed and the function returns
 * DISQUE_ERR. */
int registerJob(job *j) {
    int retval = dictAdd(server.jobs, j->id, NULL);
    return (retval == DICT_OK) ? DISQUE_OK : DISQUE_ERR;
}

/* Lookup a job by ID. */
job *lookupJob(char *id) {
    struct dictEntry *de = dictFind(server.jobs, id);

    return de ? dictGetKey(de) : NULL;
}

/* ---------------------------  Jobs serialization -------------------------- */

/* Serialize an SDS string as a little endian 32 bit count followed
 * by the bytes representing the string. The serialized string is
 * written to the memory pointed by 'p'. The return value of the function
 * is the original 'p' advanced of 4 + sdslen(s) bytes, in order to
 * be ready to store the next value to serialize. */
char *serializeSdsString(char *p, sds s) {
    size_t len = s ? sdslen(s) : 0;
    uint32_t count = intrev32ifbe(len);

    memcpy(p,&count,sizeof(count));
    if (s) memcpy(p+sizeof(count),s,len);
    return p + sizeof(count) + len;
}

sds serializeJob(job *j) {
    size_t len;
    sds msg;
    struct job *sj;
    char *p;
    uint32_t count;

    len = 4;                    /* Prefixed length of the serialized bytes. */
    len += JOB_STRUCT_SER_LEN;  /* Structure header directly serializable. */
    len += 4;                   /* Queue name length field. */
    len += j->queue ? sdslen(j->queue->ptr) : 0; /* Queue name bytes. */
    len += 4;                   /* Body length field. */
    len += j->body ? sdslen(j->body) : 0; /* Body bytes. */
    len += 4;                   /* Node IDs (that may have a copy) count. */
    len += dictSize(j->nodes_delivered) * DISQUE_CLUSTER_NAMELEN;

    /* Total serialized length prefix. */
    msg = sdsnewlen(NULL,len);
    count = intrev32ifbe(len);
    memcpy(msg,&count,sizeof(count));

    /* The serializable part of the job structure is copied, and fields
     * fixed to be little endian (no op in little endian CPUs). */
    sj = (job*) (msg+4);
    memcpy(sj,j,JOB_STRUCT_SER_LEN);
    memrev16ifbe(&sj->repl);
    memrev64ifbe(&sj->ctime);
    memrev32ifbe(&sj->etime);
    memrev32ifbe(&sj->qtime);
    memrev32ifbe(&sj->rtime);

    /* p now points to the start of the variable part of the serialization. */
    p = msg + 4 + JOB_STRUCT_SER_LEN;

    /* Queue name is 4 bytes prefixed len in little endian + actual bytes. */
    p = serializeSdsString(p,j->queue->ptr);

    /* Body is 4 bytes prefixed len in little endian + actual bytes. */
    p = serializeSdsString(p,j->body);

    /* Node IDs that may have a copy of the message: 4 bytes count in little
     * endian plus (count * JOB_ID_SIZE) bytes. */
    count = intrev32ifbe(dictSize(j->nodes_delivered));
    memcpy(p,&count,sizeof(count));
    p += sizeof(count);

    dictIterator *di = dictGetSafeIterator(j->nodes_delivered);
    dictEntry *de;
    while((de = dictNext(di)) != NULL) {
        clusterNode *node = dictGetVal(de);
        memcpy(p,node->name,DISQUE_CLUSTER_NAMELEN);
        p += DISQUE_CLUSTER_NAMELEN;
    }

    /* Make sure we wrote exactly the intented number of bytes. */
    serverAssert(len == (size_t)(p-msg));
    return msg;
}

/* -------------------------  Jobs cluster functions ------------------------ */

/* This function sends a DELJOB message to all the nodes that may have
 * a copy of the job, in order to trigger deletion of the job.
 * It is used when an ADDJOB command time out to unregister (in a best
 * effort way, without gurantees) the job, and in the ACKs grabage
 * collection procedure.
 *
 * This function also unregisters and releases the job from the local
 * node.
 *
 * The function is best effort, and does not need to *guarantee* that the
 * specific property that after it gets called, no copy of the job is found
 * on the cluster. It just attempts to avoid useless multiple deliveries,
 * and to free memory of jobs that are already processed or that were never
 * confirmed to the producer.
 */
void deleteJobFromCluster(job *j) {
    /* TODO */
    /* Send DELJOB message to the right nodes. */
    /* Unregister the job. */
    /* Free the job. */
}

/* Send the specified job to 'count' additional replicas, and populate
 * the job delivered list accordingly. */
void replicateJobInCluster(job *j, int count, int ask_for_reply) {
}

/* --------------------------  Jobs related commands ------------------------ */

/* This is called by unblockClient() to perform the cleanup of a client
 * blocked by ADDJOB. Never call it directly, call unblockClient()
 * instead. */
void unblockClientWaitingJobRepl(client *c) {
    /* If the job is still waiting for synchronous replication, but the client
     * waiting it gets freed or reaches the timeout, we unblock the client and
     * forget about the job. */
    if (c->bpop.job->state == JOB_STATE_WAIT_REPL)
        deleteJobFromCluster(c->bpop.job);
    c->bpop.job = NULL;
}

/* Return a simple string reply with the Job ID. */
void addReplyJobID(client *c, job *j) {
    addReplyStatusLength(c,j->id,JOB_ID_LEN);
}

/* ADDJOB queue job timeout [REPLICATE <n>] [TTL <sec>] [RETRY <sec>] [ASYNC] */
void addjobCommand(client *c) {
    long long replicate = 3;
    long long ttl = 3600*24;
    long long retry = -1;
    mstime_t timeout;
    int j, retval;
    int async = 0;  /* Asynchronous request? */
    static uint64_t prev_ctime = 0;

    /* Parse args. */
    for (j = 4; j < c->argc; j++) {
        char *opt = c->argv[j]->ptr;
        int lastarg = j == c->argc-1;
        if (!strcasecmp(opt,"replicate") && !lastarg) {
            retval = getLongLongFromObject(c->argv[j+1],&replicate);
            if (retval != DISQUE_OK || replicate <= 0 || replicate > 65535) {
                addReplyError(c,"REPLICATE must be between 1 and 65535");
                return;
            }
            j++;
        } else if (!strcasecmp(opt,"ttl") && !lastarg) {
            retval = getLongLongFromObject(c->argv[j+1],&ttl);
            if (retval != DISQUE_OK || ttl <= 0) {
                addReplyError(c,"TTL must be a number > 0");
                return;
            }
            j++;
        } else if (!strcasecmp(opt,"retry") && !lastarg) {
            retval = getLongLongFromObject(c->argv[j+1],&retry);
            if (retval != DISQUE_OK || retry < 0) {
                addReplyError(c,"RETRY count must be a non negative number");
                return;
            }
            j++;
        } else if (!strcasecmp(opt,"async")) {
            async = 1;
        } else {
            addReply(c,shared.syntaxerr);
            return;
        }
    }

    /* Parse the timeout argument. */
    if (getTimeoutFromObjectOrReply(c,c->argv[3],&timeout,UNIT_MILLISECONDS)
        != DISQUE_OK) return;

    /* REPLICATE > 1 and RETRY set to 0 does not make sense, why to replicate
     * the job if it will never try to be re-queued if case the job processing
     * is not acknowledged? */
    if (replicate > 1 && retry == 0) {
        addReplyError(c,"REPLICATE > 1 and RETRY 0 is invalid. "
                        "For at-most-once semantic (RETRY 0) use REPLICATE 1");
        return;
    }

    /* When retry is not specified, it defaults to 1/10 of the TTL. */
    if (retry == -1) {
        retry = ttl/10;
        if (retry == 0) retry = 1;
    }

    /* Check if REPLICATE can't be honoured at all. */
    if (replicate-1 > server.cluster->reachable_nodes_count) {
        addReplySds(c,
            sdsnew("-NOREPL Not enough reachable nodes "
                   "for the requested replication level\r\n"));
        return;
    }

    /* Create a new job. */
    job *job = createJob(NULL,JOB_STATE_WAIT_REPL,ttl);
    job->queue = c->argv[1];
    incrRefCount(c->argv[1]);
    job->repl = replicate;

    /* Job ctime is milliseconds * 1000000. Jobs created in the same
     * millisecond gets an incremental ctime. The ctime is used to sort
     * queues, so we have some weak sorting semantics for jobs: non-requeued
     * jobs are delivered roughly in the order they are added into a given
     * node. */
    job->ctime = mstime()*1000000;
    if (job->ctime == prev_ctime) job->ctime++;
    prev_ctime = job->ctime;

    job->etime = job->ctime + ttl;
    job->qtime = 0; /* Will be updated by queueAddjob(). */
    job->rtime = retry;
    job->body = sdsdup(c->argv[2]->ptr);

    if (registerJob(job) == DISQUE_ERR) {
        /* A job ID with the same name? Practically impossible but
         * let's handle it to trap possible bugs in a cleaner way. */
        serverLog(DISQUE_WARNING,"ID already existing in ADDJOB command!");
        freeJob(job);
        addReplyError(c,"Internal error creating the job, check server logs");
        return;
    }

    /* If the replication factor is > 1, send REPLJOB messages to REPLICATE-1
     * nodes. */
    if (replicate > 1) {
        int ask_for_reply = !async;
        replicateJobInCluster(job,replicate-1,ask_for_reply);
    }

    /* For replicated messages where ASYNC option was not asked, block
     * the client, and wait for acks. Otherwise if no synchronous replication
     * is used, or ASYNC option was enabled, we just queue the job and
     * return to the client ASAP.
     *
     * Note that for REPLICATE > 1 and ASYNC the replication process is
     * best effort. */
    if (replicate > 1 && !async) {
        c->bpop.timeout = timeout;
        c->bpop.job = job;
        job->state = JOB_STATE_WAIT_REPL;
        blockClient(c,DISQUE_BLOCKED_JOB_REPL);
    } else {
        queueAddJob(c->argv[1],job);
        addReplyJobID(c,job);
    }
}