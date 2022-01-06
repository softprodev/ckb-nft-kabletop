
#ifndef CKB_LUA_KABLETOP_CORE
#define CKB_LUA_KABLETOP_CORE

#include "ckb_syscalls.h"
#include "ckb_consts.h"
#include "secp256k1_lock.h"
#include "molecule/types.h"

#define MAX_SCRIPT_SIZE 32768
#define MAX_ROUND_SIZE 2048
#define MAX_CHALLENGE_DATA_SIZE 2048
#define MAX_OPERATIONS_PER_ROUND 32
#define MAX_NFT_DATA_SIZE (BLAKE160_SIZE * 256)

enum
{
    KABLETOP_SCRIPT_ERROR = 4,
    KABLETOP_ARGS_FORMAT_ERROR,
    KABLETOP_ROUND_FORMAT_ERROR,
    KABLETOP_EXCESSIVE_ROUNDS,
    KABLETOP_EXCESSIVE_WITNESS_BYTES,
    KABLETOP_WRONG_USER_ROUND,
    KABLETOP_WRONG_MODE,
    KABLETOP_WRONG_ROUND_SIGNATURE,
    KABLETOP_CHALLENGE_FORMAT_ERROR,
    KABLETOP_SETTLEMENT_FORMAT_ERROR,
    KABLETOP_RESULT_FORMAT_ERROR,
    KABLETOP_WRONG_LUA_CONTEXT_CODE,
    KABLETOP_WRONG_LUA_OPERATION_CODE,
    KABLETOP_WRONG_BATTLE_RESULT,
    KABLETOP_WRONG_SINCE
};

typedef enum
{
    MODE_SETTLEMENT,
    MODE_CHALLENGE,
    MODE_UNKNOWN
} MODE;

MODE check_mode(Kabletop *kabletop, uint8_t challenge_data[2][MAX_CHALLENGE_DATA_SIZE])
{
    uint8_t expect_lock_hash[BLAKE2B_BLOCK_SIZE];
    uint64_t len = BLAKE2B_BLOCK_SIZE;
    ckb_load_cell_by_field(expect_lock_hash, &len, 0, 0, CKB_SOURCE_GROUP_INPUT, CKB_CELL_FIELD_LOCK_HASH);

    // search outputs by input's lock_hash
    uint8_t lock_hash[BLAKE2B_BLOCK_SIZE];
    uint8_t find = 0;
    for (size_t i = 0; 1; ++i)
    {
        int ret = ckb_load_cell_by_field(lock_hash, &len, 0, i, CKB_SOURCE_OUTPUT, CKB_CELL_FIELD_LOCK_HASH);
        if (ret == CKB_INDEX_OUT_OF_BOUND)
        {
            break;
        }
        if (ret != CKB_SUCCESS)
        {
            return MODE_UNKNOWN;
        }
        if (memcmp(lock_hash, expect_lock_hash, BLAKE2B_BLOCK_SIZE) == 0)
        {
            if (find == 1)
            {
                return MODE_UNKNOWN;
            }
            len = MAX_CHALLENGE_DATA_SIZE;
            ckb_load_cell_data(challenge_data[1], &len, 0, i, CKB_SOURCE_OUTPUT);
            if (len > MAX_CHALLENGE_DATA_SIZE)
            {
                return MODE_UNKNOWN;
            }
            kabletop->output_challenge.ptr = challenge_data[1];
            kabletop->output_challenge.size = len;
            if (MolReader_Challenge_verify(&kabletop->output_challenge, false) != MOL_OK)
            {
                return MODE_UNKNOWN;
            }
            find = 1;
        }
    }
    // check if there remained challenge data in the input cell data
    len = MAX_CHALLENGE_DATA_SIZE;
    ckb_load_cell_data(challenge_data[0], &len, 0, 0, CKB_SOURCE_GROUP_INPUT);
    kabletop->input_challenge.ptr = NULL;
    if (0 < len && len < MAX_CHALLENGE_DATA_SIZE)
    {
        kabletop->input_challenge.ptr = challenge_data[0];
        kabletop->input_challenge.size = len;
    }
    if (find == 1)
    {
        // ensure offset in output_challenge must be greator than the input's one
        if (len != MAX_CHALLENGE_DATA_SIZE
            && _round_offset(kabletop, output) <= _round_offset(kabletop, input))
        {
            return MODE_UNKNOWN;
        }
        return MODE_CHALLENGE;
    }
    else
    {
        // ensure total round count from witnesses must be greator than the input's round offset
        if (len != MAX_CHALLENGE_DATA_SIZE
            && kabletop->round_count <= _round_offset(kabletop, input))
        {
            return MODE_UNKNOWN;
        }
        return MODE_SETTLEMENT;
    }
}

int check_last_round_signature(Kabletop *k, const uint8_t *expect_signature)
{
    uint64_t len = MAX_ROUND_SIZE;
    uint8_t witness[MAX_ROUND_SIZE];
    int ret = CKB_SUCCESS;
    int offset = ckb_calculate_inputs_len();
    CHECK_RET(ckb_load_witness(witness, &len, 0, offset + k->round_count - 1, CKB_SOURCE_INPUT));
    if (len > MAX_ROUND_SIZE)
    {
        return ERROR_ENCODING;
    }
    mol_seg_t lock_seg;
    CHECK_RET(extract_witness_lock(witness, len, &lock_seg));
    if (memcmp(expect_signature, lock_seg.ptr, SIGNATURE_SIZE) != 0)
    {
        return KABLETOP_WRONG_ROUND_SIGNATURE;
    }
    return CKB_SUCCESS;
}

int check_result(Kabletop *kabletop, int winner, const uint64_t ckbs[3], MODE mode)
{
    uint64_t user1_ckb   = ckbs[USER_1];
    uint64_t user2_ckb   = ckbs[USER_2];
    uint64_t funding_ckb = ckbs[USER_KABLETOP];
    uint64_t staking_ckb = _user_staking_ckb(kabletop);

    // check input SINCE wether matches the requirement when settling on no-winner result
    if (winner == 0 && mode == MODE_SETTLEMENT)
    {
        uint8_t user_type = _input_challenge_user_type(kabletop);
        if (user_type == 0 || user_type == kabletop->signer)
        {
            return KABLETOP_WRONG_BATTLE_RESULT;
        }
        uint64_t since;
        uint64_t len = sizeof(uint64_t);
        ckb_load_input_by_field(&since, &len, 0, 0, CKB_SOURCE_GROUP_INPUT, CKB_INPUT_FIELD_SINCE);
        uint64_t blocknumber = _begin_blocknumber(kabletop);
        uint64_t round_count = _round_offset(kabletop, input) + 1;
        if (round_count > 30) round_count = 30;
        if (round_count < 5)  round_count = 5;
        if (since < blocknumber + round_count * round_count)
        {
            return KABLETOP_WRONG_SINCE;
        }
        winner = kabletop->signer;
    }

    switch (winner)
    {
        // user1 is winner
        case USER_1: 
        {
            ckb_debug("winner = user1");
            if (user1_ckb - user2_ckb > funding_ckb - staking_ckb * 2
                || user1_ckb + user2_ckb < staking_ckb * 2)
            {
                return KABLETOP_RESULT_FORMAT_ERROR;
            }
            return CKB_SUCCESS;
        }
        // user2 is winner
        case USER_2:
        {
            ckb_debug("winner = user2");
            if (user2_ckb - user1_ckb > funding_ckb - staking_ckb * 2
                || user2_ckb + user1_ckb < staking_ckb * 2)
            {
                return KABLETOP_RESULT_FORMAT_ERROR;
            }
            return CKB_SUCCESS;
        }
        // lua global variable "_winner" is set to a wrong value while in settlement mode
        default:
        {
            if (mode == MODE_SETTLEMENT)
            {
                return KABLETOP_WRONG_LUA_OPERATION_CODE;
            }
        }
    }

    return CKB_SUCCESS;
}

int verify_lock_args(Kabletop *kabletop, uint8_t script[MAX_SCRIPT_SIZE])
{
    // fetch kabletop params from context and point to "args" field
    uint64_t len = MAX_SCRIPT_SIZE;
    int ret = ckb_load_script(script, &len, 0);
    if (ret != CKB_SUCCESS || len > MAX_SCRIPT_SIZE)
    {
        return KABLETOP_SCRIPT_ERROR;
    }
    mol_seg_t script_seg;
    script_seg.ptr = (uint8_t *)script;
    script_seg.size = len;
    if (MolReader_Script_verify(&script_seg, false) != MOL_OK)
    {
        return KABLETOP_SCRIPT_ERROR;
    }
    mol_seg_t args_seg = MolReader_Script_get_args(&script_seg);
    kabletop->args = MolReader_Bytes_raw_bytes(&args_seg);
    if (MolReader_Args_verify(&kabletop->args, false) != MOL_OK)
    {
        return KABLETOP_ARGS_FORMAT_ERROR;
    }
    // CAUTION: there should be some examination to ensure both users' nft count must
    // be equal to _user_deck_size(kabletop), but this script is filled in lock_script
    // and will not run while creating the kabletop-cell, so the examination should be
    // implemented off-chain, especially by two of kabletop game clients
    return CKB_SUCCESS;
}

int verify_witnesses(Kabletop *kabletop, uint8_t witnesses[MAX_ROUND_COUNT][MAX_ROUND_SIZE])
{
    uint8_t pubkey_hash[BLAKE160_SIZE];
    int ret = CKB_SUCCESS;
    CHECK_RET(get_secp256k1_blake160_sighash_all(pubkey_hash, 0, CKB_SOURCE_GROUP_INPUT));

    // any one of users should match signature
    if (memcmp(pubkey_hash, _user1_pkhash(kabletop), BLAKE160_SIZE) == 0)
    {
        kabletop->signer = USER_1;
    }
    else if (memcmp(pubkey_hash, _user2_pkhash(kabletop), BLAKE160_SIZE) == 0)
    {
        kabletop->signer = USER_2;
    }
    else
    {
        return ERROR_PUBKEY_BLAKE160_HASH;
    }

    int s = ckb_calculate_inputs_len();
    int e = s;
    uint64_t len = MAX_ROUND_SIZE;
    while (ckb_load_witness(witnesses[e-s], &len, 0, e, CKB_SOURCE_INPUT) != CKB_INDEX_OUT_OF_BOUND)
    {
        if (len > MAX_ROUND_SIZE)
        {
            return KABLETOP_EXCESSIVE_WITNESS_BYTES;
        }
        e += 1;
    }
    kabletop->round_count = e - s;
    if (kabletop->round_count > MAX_ROUND_COUNT || kabletop->round_count == 0)
    {
        return KABLETOP_EXCESSIVE_ROUNDS;
    }

    // check round signatures, always start from lock_hash and capacity
    uint8_t lock_hash[BLAKE2B_BLOCK_SIZE];
    len = BLAKE2B_BLOCK_SIZE;
    ckb_load_cell_by_field(lock_hash, &len, 0, 0, CKB_SOURCE_GROUP_INPUT, CKB_CELL_FIELD_LOCK_HASH);

    uint64_t capacity = 0;
    len = sizeof(uint64_t);
    ckb_load_cell_by_field(&capacity, &len, 0, 0, CKB_SOURCE_GROUP_INPUT, CKB_CELL_FIELD_CAPACITY);

    blake2b_state blake2b_ctx;
    blake2b_init(&blake2b_ctx, BLAKE2B_BLOCK_SIZE);
    blake2b_update(&blake2b_ctx, lock_hash, BLAKE2B_BLOCK_SIZE);
    blake2b_update(&blake2b_ctx, &capacity, sizeof(uint64_t));

    mol_seg_t signature_seg;
    uint8_t message[BLAKE2B_BLOCK_SIZE];
    for (uint8_t i = 0; i < kabletop->round_count; ++i)
    {
        if (i > 0)
        {
            blake2b_init(&blake2b_ctx, BLAKE2B_BLOCK_SIZE);
            blake2b_update(&blake2b_ctx, message, BLAKE2B_BLOCK_SIZE);
            blake2b_update(&blake2b_ctx, signature_seg.ptr, SIGNATURE_SIZE);
        }
        uint8_t *witness = witnesses[i];
        len = MAX_ROUND_SIZE;
        ckb_load_witness(witness, &len, 0, i + s, CKB_SOURCE_INPUT);
        // extract round signature from extra witness lock
        CHECK_RET(extract_witness_lock(witness, len, &signature_seg));
        if (signature_seg.size != SIGNATURE_SIZE)
        {
            return ERROR_ARGUMENTS_LEN;
        }
        // extract round from extra witness input_type
        CHECK_RET(extract_witness_input_type(witness, len, &kabletop->rounds[i]));
        if (MolReader_Round_verify(&kabletop->rounds[i], false) != MOL_OK
            || _operations_count(kabletop, i) > MAX_OPERATIONS_PER_ROUND)
        {
            return KABLETOP_ROUND_FORMAT_ERROR;
        }
        // complete signature message with round data
        blake2b_update(&blake2b_ctx, kabletop->rounds[i].ptr, kabletop->rounds[i].size);
        blake2b_final(&blake2b_ctx, message, BLAKE2B_BLOCK_SIZE);
        // CAUTION: the method "get_secp256k1_pubkey_blake160" is way too EXPENSIVE, so we just check
        // two signatures from last TWO rounds of this game which already contain both two users' confirmation
        if (i + 2 >= kabletop->round_count)
        {
            // recover pubkey blake160 hash
            CHECK_RET(get_secp256k1_pubkey_blake160(pubkey_hash, signature_seg.ptr, message));
            // check round owner
            if ((_user_type(kabletop, i) == USER_1 && memcmp(pubkey_hash, _user2_pkhash(kabletop), BLAKE160_SIZE) != 0)
                || (_user_type(kabletop, i) == USER_2 && memcmp(pubkey_hash, _user1_pkhash(kabletop), BLAKE160_SIZE) != 0))
            {
                return KABLETOP_WRONG_USER_ROUND;
            }
        }
        // fill round random seed from first 16 bytes of round signature
        memcpy(kabletop->seeds[i].randomseed, signature_seg.ptr, sizeof(uint64_t) * 2);
    }
    return CKB_SUCCESS;
}

int verify_settlement_mode(Kabletop *kabletop, uint64_t capacities[3])
{
    const uint8_t *expect_code_hash = _lock_code_hash(kabletop);
    uint8_t lock_script[MAX_SCRIPT_SIZE];
    uint8_t user_checked[2] = {0, 0};
    int ret = CKB_SUCCESS;
    for (size_t i = 0; 1; ++i)
    {
        uint64_t len = MAX_SCRIPT_SIZE;
        // filter cell by lock_script's code_hash
        ret = ckb_load_cell_by_field(lock_script, &len, 0, i, CKB_SOURCE_OUTPUT, CKB_CELL_FIELD_LOCK);
        if (ret == CKB_INDEX_OUT_OF_BOUND)
        {
            break;
        }
        if (ret != CKB_SUCCESS || len > MAX_SCRIPT_SIZE)
        {
            return ERROR_ENCODING;
        }
        mol_seg_t script;
        script.ptr = lock_script;
        script.size = len;
        mol_seg_t code_hash = MolReader_Script_get_code_hash(&script);
        if (code_hash.size != BLAKE2B_BLOCK_SIZE)
        {
            return ERROR_ENCODING;
        }
        if (memcmp(code_hash.ptr, expect_code_hash, BLAKE2B_BLOCK_SIZE) != 0)
        {
            continue;
        }
        // check lock_args difference between input and output
        mol_seg_t lock_args = MolReader_Script_get_args(&script);
        lock_args = MolReader_Bytes_raw_bytes(&lock_args);
        if (memcmp(lock_args.ptr, _user1_pkhash(kabletop), BLAKE160_SIZE) == 0 && user_checked[0] == 0)
        {
            user_checked[0] = 1;
            len = sizeof(uint64_t);
            CHECK_RET(ckb_load_cell_by_field(&capacities[USER_1], &len, 0, i, CKB_SOURCE_OUTPUT, CKB_CELL_FIELD_CAPACITY));
        }
        else if (memcmp(lock_args.ptr, _user2_pkhash(kabletop), BLAKE160_SIZE) == 0 && user_checked[1] == 0)
        {
            user_checked[1] = 1;
            len = sizeof(uint64_t);
            CHECK_RET(ckb_load_cell_by_field(&capacities[USER_2], &len, 0, i, CKB_SOURCE_OUTPUT, CKB_CELL_FIELD_CAPACITY));
        }
    }
    // check wether contain both of two users output cells
    if (user_checked[0] == 0 || user_checked[1] == 0)
    {
        return KABLETOP_SETTLEMENT_FORMAT_ERROR;
    }
    uint64_t len = sizeof(uint64_t);
    CHECK_RET(ckb_load_cell_by_field(&capacities[USER_KABLETOP], &len, 0, 0, CKB_SOURCE_GROUP_INPUT, CKB_CELL_FIELD_CAPACITY));
    return CKB_SUCCESS;
}

int verify_challenge_mode(Kabletop *kabletop)
{
    if (_round_offset(kabletop, output) != kabletop->round_count - 1)
    {
        return KABLETOP_CHALLENGE_FORMAT_ERROR;
    }
    int ret = check_last_round_signature(kabletop, _signature(kabletop, output));
    if (ret != CKB_SUCCESS)
    {
        return KABLETOP_CHALLENGE_FORMAT_ERROR;
    }
    mol_seg_t last_round = kabletop->rounds[kabletop->round_count - 1];
    mol_seg_t challenge_round = _round(kabletop, output);
    if (last_round.size != challenge_round.size
        || memcmp(last_round.ptr, challenge_round.ptr, last_round.size) != 0)
    {
        return KABLETOP_CHALLENGE_FORMAT_ERROR;
    }
    return CKB_SUCCESS;
}

#endif
