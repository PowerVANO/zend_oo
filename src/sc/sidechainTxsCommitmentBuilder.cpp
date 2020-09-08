#include <sc/sidechainTxsCommitmentBuilder.h>
#include <primitives/block.h>
#include <primitives/transaction.h>
#include <primitives/certificate.h>

uint256 SidechainTxsCommitmentBuilder::getMerkleRootHash(const std::vector<uint256>& vInput)
{
    std::vector<uint256> vTempMerkleTree = vInput;
    return CBlock::BuildMerkleTree(vTempMerkleTree, vInput.size());
}

const unsigned char SidechainTxsCommitmentBuilder::zeros[SC_FIELD_SIZE] = {'\0'};

SidechainTxsCommitmentBuilder::SidechainTxsCommitmentBuilder():
        mScMerkleTreeLeavesFt(), mScMerkleTreeLeavesBtr(),
        mScCerts(), sScIds(), emptyField(zendoo_deserialize_field(zeros))
{}

SidechainTxsCommitmentBuilder::~SidechainTxsCommitmentBuilder()
{
    zendoo_field_free(const_cast<field_t*>(emptyField));
}

#ifdef BITCOIN_TX
void SidechainTxsCommitmentBuilder::add(const CTransaction& tx) { return; }
void SidechainTxsCommitmentBuilder::add(const CScCertificate& cert) { return; }
uint256 SidechainTxsCommitmentBuilder::getCommitment() { return uint256(); }
#else
void SidechainTxsCommitmentBuilder::add(const CTransaction& tx)
{
    if (!tx.IsScVersion())
        return;

    unsigned int ccOutIdx = 0;
    LogPrint("sc", "%s():%d -getting leaves for vsc out\n", __func__, __LINE__);
    for(; ccOutIdx < tx.GetVscCcOut().size(); ++ccOutIdx)
        addCrosschainOutput(tx.GetHash(), tx.GetVscCcOut()[ccOutIdx], ccOutIdx, mScMerkleTreeLeavesFt);

    LogPrint("sc", "%s():%d -getting leaves for vft out\n", __func__, __LINE__);
    for(; ccOutIdx < tx.GetVftCcOut().size(); ++ccOutIdx)
        addCrosschainOutput(tx.GetHash(), tx.GetVftCcOut()[ccOutIdx], ccOutIdx, mScMerkleTreeLeavesFt);

    LogPrint("sc", "%s():%d - nIdx[%d]\n", __func__, __LINE__, ccOutIdx);
}

void SidechainTxsCommitmentBuilder::add(const CScCertificate& cert)
{
    sScIds.insert(cert.GetScId());
    mScCerts[cert.GetScId()] = mapCertToField(cert.GetHash());
}

uint256 SidechainTxsCommitmentBuilder::getCommitment()
{
    std::vector<uint256> vSortedScLeaves;

    // set of scid is ordered
    for (const auto& scid : sScIds)
    {
        field_t* ftRootField = nullptr;
        auto itFt = mScMerkleTreeLeavesFt.find(scid);
        if (itFt != mScMerkleTreeLeavesFt.end() )
        {
            unsigned int ftTreeHeight = static_cast<int>(ceil(log2f(static_cast<float>(itFt->second.size())))) + 1;
            auto ftTree = ZendooGingerRandomAccessMerkleTree(ftTreeHeight);
            for(field_t* & leaf: itFt->second)
            {
                ftTree.append(leaf);
                zendoo_field_free(leaf);
            }

            ftTree.finalize_in_place();
            ftRootField = ftTree.root(); //deep-copied. To be explicitly freed later on
        }

        field_t* btrRootField = nullptr;
        auto itBtr = mScMerkleTreeLeavesBtr.find(scid);
        if (itBtr != mScMerkleTreeLeavesBtr.end() )
        {
            unsigned int btrTreeHeight = static_cast<int>(ceil(log2f(static_cast<float>(itBtr->second.size())))) + 1;
            auto btrTree = ZendooGingerRandomAccessMerkleTree(btrTreeHeight);
            for(field_t* & leaf: itBtr->second)
            {
                btrTree.append(leaf);
                zendoo_field_free(leaf);
            }

            btrTree.finalize_in_place();
            btrRootField = btrTree.root(); //root deep-copied. To be explicitly freed later on
        }

        field_t* certRootField = nullptr;
        auto itCert = mScCerts.find(scid);
        if (itCert != mScCerts.end() )
        {
            certRootField = itCert->second; //for symmetry's sake
        }

        unsigned int sidechainTreeHeight = 2 + 1;
        auto sidechainTree = ZendooGingerRandomAccessMerkleTree(sidechainTreeHeight);

        sidechainTree.append((ftRootField == nullptr)? emptyField : ftRootField);     //there may be no fwds
        sidechainTree.append((btrRootField == nullptr)? emptyField : btrRootField);   //there may be no btrs
        sidechainTree.append((certRootField == nullptr)? emptyField : certRootField); //there may be no cert

        field_t* scIdField = mapCertToField(scid);
        sidechainTree.append((scIdField == nullptr)? emptyField : scIdField); //guard against faulty map to field

        sidechainTree.finalize();
        field_t * sidechainTreeRoot = sidechainTree.root();
        vSortedScLeaves.push_back(mapFieldToHash((sidechainTreeRoot == nullptr)? emptyField : sidechainTreeRoot));

        if (ftRootField != emptyField) zendoo_field_free(ftRootField);
        if (btrRootField != emptyField) zendoo_field_free(btrRootField);
        if (certRootField != emptyField) zendoo_field_free(certRootField);
        if (scIdField != emptyField) zendoo_field_free(scIdField);
        if (sidechainTreeRoot != emptyField) zendoo_field_free(sidechainTreeRoot);
    }

    return getMerkleRootHash(vSortedScLeaves);
}
#endif

field_t* SidechainTxsCommitmentBuilder::mapScTxToField(const uint256& ccoutHash, const uint256& txHash, unsigned int outPos)
{
    // 1- Map relevant inputs to field element, padding with zeros till SC_FIELD_SIZE bytes
    // Currently re-using SHA256 to pub all relevant data together since...
    static_assert(sizeof(uint256) < SC_FIELD_SIZE);

    uint256 res = Hash(BEGIN(ccoutHash), END(ccoutHash),
                       BEGIN(txHash),    END(txHash),
                       BEGIN(outPos),    END(outPos) );

    unsigned char hash[SC_FIELD_SIZE] = {};
    std::string resHex = res.GetHex();
    memcpy(hash, resHex.append(SC_FIELD_SIZE - resHex.size(), '\0').c_str(), SC_FIELD_SIZE);

    //generate field element. MISSING null-check
    field_t* field = zendoo_deserialize_field(hash);
    return field;
}

field_t* SidechainTxsCommitmentBuilder::mapCertToField(const uint256& certHash)
{
    static_assert(sizeof(uint256) < SC_FIELD_SIZE);
    unsigned char hash[SC_FIELD_SIZE] = {};
    std::string resHex = certHash.ToString();
    memcpy(hash, resHex.append(SC_FIELD_SIZE - resHex.size(), '\0').c_str(), SC_FIELD_SIZE);

    //generate field element. MISSING null-check
    field_t* field = zendoo_deserialize_field(hash);
    return field;
}

uint256 SidechainTxsCommitmentBuilder::mapFieldToHash(const field_t* pField)
{
    if (pField == nullptr)
        throw;

    unsigned char field_bytes[SC_FIELD_SIZE];
    zendoo_serialize_field(pField, field_bytes);

    uint256 res = Hash(BEGIN(field_bytes), END(field_bytes));
    return res;
}
