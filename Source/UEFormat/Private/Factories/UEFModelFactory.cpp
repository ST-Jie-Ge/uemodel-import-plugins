// Copyright © 2025 Marcel K. All rights reserved.

#include "Factories/UEFModelFactory.h"
#include "ImportUtils/SkelImport.h"
#include "StaticMeshResources.h"
#include "AssetRegistryModule.h"           
#include "Engine/SkeletalMesh.h"
#include "Engine/SkeletalMeshSocket.h"
#include "Engine/StaticMesh.h"
#include "MeshUtilities.h"                 
#include "RawMesh.h"                       
#include "Animation/MorphTarget.h"
#include "Rendering/SkeletalMeshModel.h"   
#include "SkeletalMeshTypes.h"


const float ImportScale = 1.0f;


UEFModelFactory::UEFModelFactory(const FObjectInitializer& ObjectInitializer)
    : Super(ObjectInitializer)
{
    Formats.Add(TEXT("uemodel; UEMODEL Mesh File"));
    SupportedClass = UObject::StaticClass();
    bCreateNew     = false;
    bEditorImport  = true;
}


UObject* UEFModelFactory::FactoryCreateFile(
    UClass* Class, UObject* Parent, FName Name, EObjectFlags Flags,
    const FString& Filename, const TCHAR* Params, FFeedbackContext* Warn,
    bool& bOutOperationCanceled)
{
    UEFModelReader Data(Filename);
    if (!Data.Read() || Data.LODs.Num() == 0)
        return nullptr;

    if (Data.Skeleton.Bones.Num() > 0)
    {
        USkeletalMesh* SkeletalMesh = CreateSkeletalMesh(Data.LODs, Data.Skeleton, Parent, Name, Flags);
        if (!SkeletalMesh) return nullptr;
        SkeletalMesh->PostEditChange();
        FAssetRegistryModule::AssetCreated(SkeletalMesh);
        return SkeletalMesh;
    }
    else
    {
        UStaticMesh* StaticMesh = CreateStaticMesh(Data.LODs, Parent, Name, Flags);
        if (!StaticMesh) return nullptr;
        StaticMesh->PostEditChange();
        FAssetRegistryModule::AssetCreated(StaticMesh);
        return StaticMesh;
    }
}

UStaticMesh* UEFModelFactory::CreateStaticMesh(TArray<FLODData>& LODData, UObject* Parent, FName Name, EObjectFlags Flags)
{
    UStaticMesh* StaticMesh = NewObject<UStaticMesh>(Parent->GetOutermost(), Name, Flags);
    StaticMesh->PreEditChange(nullptr);

    for (int32 LodIndex = 0; LodIndex < LODData.Num(); ++LodIndex)
    {
        FLODData& Data = LODData[LodIndex];

        if (Data.Vertices.Num() == 0 || Data.Indices.Num() == 0 || (Data.Indices.Num() % 3) != 0)
        {
            UE_LOG(LogTemp, Error, TEXT("UEFormat: invalid static mesh topology in LOD %d (Vertices=%d Indices=%d)"),
                LodIndex, Data.Vertices.Num(), Data.Indices.Num());
            return nullptr;
        }

        FStaticMeshSourceModel& SourceModel = StaticMesh->AddSourceModel();
        SourceModel.BuildSettings.bRecomputeNormals    = false;
        SourceModel.BuildSettings.bRecomputeTangents   = false;
        SourceModel.BuildSettings.bRemoveDegenerates   = false;
        SourceModel.BuildSettings.bGenerateLightmapUVs = false;

        FRawMesh RawMesh;

        RawMesh.VertexPositions.Reserve(Data.Vertices.Num());
        for (const FVector& V : Data.Vertices)
            RawMesh.VertexPositions.Add(V * ImportScale);

        const int32 NumUVChannels = FMath::Max(1, Data.TextureCoordinates.Num());


        const int32 NumWedges = Data.Indices.Num();
        RawMesh.WedgeIndices.Reserve(NumWedges);
        RawMesh.WedgeTangentX.Reserve(NumWedges);
        RawMesh.WedgeTangentY.Reserve(NumWedges);
        RawMesh.WedgeTangentZ.Reserve(NumWedges);
        for (int32 ch = 0; ch < NumUVChannels; ++ch)
            RawMesh.WedgeTexCoords[ch].Reserve(NumWedges);
        if (Data.VertexColors.Num() > 0)
            RawMesh.WedgeColors.Reserve(NumWedges);

        for (int32 i = 0; i < NumWedges; i++)
        {
            const int32 VIdx = Data.Indices[i];
            if (!Data.Vertices.IsValidIndex(VIdx))
            {
                UE_LOG(LogTemp, Error, TEXT("UEFormat: invalid static-mesh vertex index in LOD %d (Index=%d Vertex=%d Vertices=%d)"),
                    LodIndex, i, VIdx, Data.Vertices.Num());
                return nullptr;
            }
            RawMesh.WedgeIndices.Add(VIdx);

            FVector Normal   = FVector::ZeroVector;
            FVector Tangent  = FVector::ZeroVector;
            float   BinSign  = 1.f;
            if (Data.Normals.IsValidIndex(VIdx))
            {
                const FVector4& N4 = Data.Normals[VIdx];
                BinSign = N4.X;
                Normal  = FVector(N4.Y, N4.Z, N4.W);
            }
            if (Data.Tangents.IsValidIndex(VIdx))
                Tangent = Data.Tangents[VIdx];

            FVector Binormal = FVector::CrossProduct(Normal, Tangent) * BinSign;

            RawMesh.WedgeTangentX.Add(Tangent);
            RawMesh.WedgeTangentY.Add(Binormal);
            RawMesh.WedgeTangentZ.Add(Normal);

            for (int32 ch = 0; ch < NumUVChannels; ++ch)
            {
                FVector2D UV = FVector2D::ZeroVector;
                if (Data.TextureCoordinates.IsValidIndex(ch) && Data.TextureCoordinates[ch].IsValidIndex(VIdx))
                    UV = Data.TextureCoordinates[ch][VIdx];
                RawMesh.WedgeTexCoords[ch].Add(UV);
            }

            if (Data.VertexColors.Num() > 0 && Data.VertexColors[0].Data.IsValidIndex(VIdx))
                RawMesh.WedgeColors.Add(Data.VertexColors[0].Data[VIdx]);
            else if (Data.VertexColors.Num() > 0)
                RawMesh.WedgeColors.Add(FColor::White);
        }

        const int32 NumFaces = NumWedges / 3;
        RawMesh.FaceMaterialIndices.Init(INDEX_NONE, NumFaces);
        RawMesh.FaceSmoothingMasks.Init(0xFFFFFFFF, NumFaces);

        for (int32 MatIdx = 0; MatIdx < Data.Materials.Num(); ++MatIdx)
        {
            const FMaterialChunk& Mat = Data.Materials[MatIdx];
            if (Mat.FirstIndex < 0 || Mat.NumFaces < 0 || (Mat.FirstIndex % 3) != 0)
            {
                UE_LOG(LogTemp, Error, TEXT("UEFormat: invalid static material range in LOD %d Material %d (FirstIndex=%d NumFaces=%d)"),
                    LodIndex, MatIdx, Mat.FirstIndex, Mat.NumFaces);
                return nullptr;
            }

            const int64 FirstFace = static_cast<int64>(Mat.FirstIndex) / 3;
            const int64 EndFace = FirstFace + static_cast<int64>(Mat.NumFaces);
            UE_LOG(LogTemp, Display, TEXT("UEFormat: static material map LOD=%d Material=%d FirstIndex=%d FirstFace=%lld NumFaces=%d TotalFaces=%d"),
                LodIndex, MatIdx, Mat.FirstIndex, FirstFace, Mat.NumFaces, NumFaces);
            if (EndFace > NumFaces)
            {
                UE_LOG(LogTemp, Error, TEXT("UEFormat: static material range exceeds index buffer in LOD %d Material %d (FirstIndex=%d NumFaces=%d TotalFaces=%d)"),
                    LodIndex, MatIdx, Mat.FirstIndex, Mat.NumFaces, NumFaces);
                return nullptr;
            }
            for (int64 f = FirstFace; f < EndFace; ++f)
            {
                const int32 FaceIndex = static_cast<int32>(f);
                if (RawMesh.FaceMaterialIndices[FaceIndex] != INDEX_NONE)
                {
                    UE_LOG(LogTemp, Error, TEXT("UEFormat: overlapping static material ranges in LOD %d at Face %d"), LodIndex, FaceIndex);
                    return nullptr;
                }
                RawMesh.FaceMaterialIndices[FaceIndex] = MatIdx;
            }
        }

        for (int32 FaceIndex = 0; FaceIndex < NumFaces; ++FaceIndex)
        {
            if (RawMesh.FaceMaterialIndices[FaceIndex] == INDEX_NONE)
            {
                UE_LOG(LogTemp, Error, TEXT("UEFormat: static face %d in LOD %d is not covered by any material"), FaceIndex, LodIndex);
                return nullptr;
            }
        }

        SourceModel.RawMeshBulkData->SaveRawMesh(RawMesh);
    }

    for (const FMaterialChunk& MatInfo : LODData[0].Materials)
    {
        FStaticMaterial Mat;
        Mat.MaterialSlotName         = FName(MatInfo.Name.c_str());
        Mat.ImportedMaterialSlotName = FName(MatInfo.Name.c_str());
        Mat.MaterialInterface        = nullptr;
        StaticMesh->GetStaticMaterials().Add(Mat);
    }

    StaticMesh->Build(false);
    StaticMesh->MarkPackageDirty();
    return StaticMesh;
}

USkeleton* UEFModelFactory::CreateSkeleton(
    const FString& Name, UObject* Parent, EObjectFlags Flags,
    FSkeletonData& Data, FReferenceSkeleton& OutRefSkeleton)
{
    FString SkeletonName = Name + TEXT("_Skeleton");
    UPackage* SkeletonPackage = CreatePackage(*FPaths::Combine(FPaths::GetPath(Parent->GetPathName()), SkeletonName));
    USkeleton* Skeleton = NewObject<USkeleton>(SkeletonPackage, FName(*SkeletonName), Flags);

    {
        FReferenceSkeletonModifier Modifier(OutRefSkeleton, Skeleton);
        OutRefSkeleton.Empty();

        for (const FBoneChunk& Bone : Data.Bones)
        {
            FTransform Transform;
            Transform.SetLocation(Bone.BonePos);
            Transform.SetRotation(Bone.BoneRot);
            Transform.SetScale3D(FVector::OneVector);

            FMeshBoneInfo Info(FName(Bone.BoneName.c_str()), Bone.BoneName.c_str(), Bone.BoneParentIndex);
            Modifier.Add(Info, Transform);
        }
    }

    for (const FSocketChunk& Sock : Data.Sockets)
    {
        USkeletalMeshSocket* NewSocket = NewObject<USkeletalMeshSocket>(Skeleton);
        NewSocket->SocketName     = FName(Sock.SocketName.c_str());
        NewSocket->BoneName       = FName(Sock.SocketParentName.c_str());
        NewSocket->RelativeLocation = Sock.SocketPos * ImportScale;
        NewSocket->RelativeRotation = Sock.SocketRot.Rotator();
        NewSocket->RelativeScale    = Sock.SocketScale;
        Skeleton->Sockets.Add(NewSocket);
    }

    return Skeleton;
}

USkeletalMesh* UEFModelFactory::CreateSkeletalMesh(
    TArray<FLODData>& LODData, FSkeletonData& SkeletonData,
    UObject* Parent, FName Name, EObjectFlags Flags)
{
    if (SkeletonData.Bones.Num() == 0)
    {
        UE_LOG(LogTemp, Error, TEXT("UEFormat: skeletal mesh has no bones"));
        return nullptr;
    }
    for (int32 BoneIndex = 0; BoneIndex < SkeletonData.Bones.Num(); ++BoneIndex)
    {
        const int32 ParentIndex = SkeletonData.Bones[BoneIndex].BoneParentIndex;
        const bool bValidRoot = BoneIndex == 0 && (ParentIndex == INDEX_NONE || ParentIndex == 0);
        const bool bValidChild = BoneIndex > 0 && ParentIndex >= 0 && ParentIndex < BoneIndex;
        if (!bValidRoot && !bValidChild)
        {
            UE_LOG(LogTemp, Error, TEXT("UEFormat: invalid skeleton parent (Bone=%d Parent=%d BoneCount=%d)"),
                BoneIndex, ParentIndex, SkeletonData.Bones.Num());
            return nullptr;
        }
    }

    USkeletalMesh* SkeletalMesh = NewObject<USkeletalMesh>(Parent->GetOutermost(), Name, Flags);
    SkeletalMesh->PreEditChange(nullptr);

    FReferenceSkeleton RefSkeleton;
    USkeleton* Skeleton = CreateSkeleton(Name.ToString(), Parent, Flags, SkeletonData, RefSkeleton);

    SkeletalMesh->SetRefSkeleton(RefSkeleton);
    SkeletalMesh->SetSkeleton(Skeleton);

    for (const FMaterialChunk& MatInfo : LODData[0].Materials)
    {
        FSkeletalMaterial Mat;
        Mat.MaterialSlotName         = FName(MatInfo.Name.c_str());
        Mat.ImportedMaterialSlotName = FName(MatInfo.Name.c_str());
        Mat.MaterialInterface        = nullptr;
        SkeletalMesh->GetMaterials().Add(Mat);
    }

    IMeshUtilities& MeshUtils = FModuleManager::Get().LoadModuleChecked<IMeshUtilities>("MeshUtilities");
    FSkeletalMeshModel* ImportedModel = SkeletalMesh->GetImportedModel();
    ImportedModel->LODModels.Empty();

    for (int32 LodIndex = 0; LodIndex < LODData.Num(); ++LodIndex)
    {
        FLODData& Data = LODData[LodIndex];

        if (Data.Vertices.Num() == 0 || Data.Indices.Num() == 0 || (Data.Indices.Num() % 3) != 0)
        {
            UE_LOG(LogTemp, Error, TEXT("UEFormat: invalid skeletal mesh topology in LOD %d (Vertices=%d Indices=%d)"),
                LodIndex, Data.Vertices.Num(), Data.Indices.Num());
            return nullptr;
        }
        if (Data.Materials.Num() > 256)
        {
            UE_LOG(LogTemp, Error, TEXT("UEFormat: LOD %d has %d materials; skeletal faces support at most 256"),
                LodIndex, Data.Materials.Num());
            return nullptr;
        }

        ImportedModel->LODModels.Add(new FSkeletalMeshLODModel());
        FSkeletalMeshLODModel& LODModel = ImportedModel->LODModels[LodIndex];

        const int32 NumUVChannels = FMath::Clamp<int32>(
            Data.TextureCoordinates.Num(),
            1,
            (int32)MAX_TEXCOORDS
        );
        LODModel.NumTexCoords = NumUVChannels;

        FSkeletalMeshImportData ImportData;

        bool bValidNormals = Data.Normals.Num() == Data.Vertices.Num();
        bool bValidTangents = Data.Tangents.Num() == Data.Vertices.Num();
        if (bValidNormals)
        {
            for (const FVector4& PackedNormal : Data.Normals)
            {
                const FVector Normal(PackedNormal.Y, PackedNormal.Z, PackedNormal.W);
                if (Normal.ContainsNaN() || Normal.SizeSquared() <= SMALL_NUMBER || !FMath::IsFinite(PackedNormal.X))
                {
                    bValidNormals = false;
                    break;
                }
            }
        }
        if (bValidTangents)
        {
            for (const FVector& Tangent : Data.Tangents)
            {
                if (Tangent.ContainsNaN() || Tangent.SizeSquared() <= SMALL_NUMBER)
                {
                    bValidTangents = false;
                    break;
                }
            }
        }
        ImportData.bHasNormals      = bValidNormals;
        ImportData.bHasTangents     = bValidNormals && bValidTangents;
        ImportData.bHasVertexColors = Data.VertexColors.Num() > 0;
        ImportData.NumTexCoords     = NumUVChannels;

        if (!ImportData.bHasNormals || !ImportData.bHasTangents)
        {
            UE_LOG(LogTemp, Warning, TEXT("UEFormat: LOD %d has incomplete/invalid tangent data; UE4 will recompute normals and tangents"), LodIndex);
        }

        ImportData.Points.Reserve(Data.Vertices.Num());
        for (const FVector& V : Data.Vertices)
            ImportData.Points.Add(V * ImportScale);

        TArray<int32> PointToRawMap;
        PointToRawMap.Reserve(Data.Vertices.Num());
        for (int32 i = 0; i < Data.Vertices.Num(); i++)
            PointToRawMap.Add(i);
        ImportData.PointToRawMap = PointToRawMap;

        ImportData.RefBonesBinary.Reserve(SkeletonData.Bones.Num());
        for (const FBoneChunk& Bone : SkeletonData.Bones)
        {
            SkeletalMeshImportData::FBone ImportBone;
            ImportBone.Name        = Bone.BoneName.c_str();
            ImportBone.Flags       = 0;
            ImportBone.NumChildren = 0;
            ImportBone.ParentIndex = Bone.BoneParentIndex;
            FTransform BoneTransform;
            BoneTransform.SetLocation(Bone.BonePos * ImportScale);
            BoneTransform.SetRotation(Bone.BoneRot);
            BoneTransform.SetScale3D(FVector::OneVector);
            ImportBone.BonePos.Transform = BoneTransform;
            ImportBone.BonePos.Length    = 0.f;
            ImportBone.BonePos.XSize     = 0.f;
            ImportBone.BonePos.YSize     = 0.f;
            ImportBone.BonePos.ZSize     = 0.f;
            ImportData.RefBonesBinary.Add(ImportBone);
        }

        ImportData.Materials.Reserve(Data.Materials.Num());
        for (const FMaterialChunk& Mat : Data.Materials)
        {
            SkeletalMeshImportData::FMaterial ImportMat;
            ImportMat.MaterialImportName = Mat.Name.c_str();
            ImportMat.Material           = nullptr;
            ImportData.Materials.Add(ImportMat);
        }

        const int32 NumFaces = Data.Indices.Num() / 3;
        TArray<int32> FaceMaterialIndices;
        FaceMaterialIndices.Init(INDEX_NONE, NumFaces);

        for (int32 MatIdx = 0; MatIdx < Data.Materials.Num(); ++MatIdx)
        {
            const FMaterialChunk& Mat = Data.Materials[MatIdx];
            if (Mat.FirstIndex < 0 || Mat.NumFaces < 0 || (Mat.FirstIndex % 3) != 0)
            {
                UE_LOG(LogTemp, Error, TEXT("UEFormat: invalid skeletal material range in LOD %d Material %d (FirstIndex=%d NumFaces=%d)"),
                    LodIndex, MatIdx, Mat.FirstIndex, Mat.NumFaces);
                return nullptr;
            }

            const int64 FirstFace = static_cast<int64>(Mat.FirstIndex) / 3;
            const int64 EndFace = FirstFace + static_cast<int64>(Mat.NumFaces);
            UE_LOG(LogTemp, Display, TEXT("UEFormat: skeletal material map LOD=%d Material=%d FirstIndex=%d FirstFace=%lld NumFaces=%d TotalFaces=%d"),
                LodIndex, MatIdx, Mat.FirstIndex, FirstFace, Mat.NumFaces, NumFaces);
            if (EndFace > NumFaces)
            {
                UE_LOG(LogTemp, Error, TEXT("UEFormat: skeletal material range exceeds index buffer in LOD %d Material %d (FirstIndex=%d NumFaces=%d TotalFaces=%d)"),
                    LodIndex, MatIdx, Mat.FirstIndex, Mat.NumFaces, NumFaces);
                return nullptr;
            }
            for (int64 FaceIndex = FirstFace; FaceIndex < EndFace; ++FaceIndex)
            {
                const int32 FaceArrayIndex = static_cast<int32>(FaceIndex);
                if (FaceMaterialIndices[FaceArrayIndex] != INDEX_NONE)
                {
                    UE_LOG(LogTemp, Error, TEXT("UEFormat: overlapping skeletal material ranges in LOD %d at Face %d"), LodIndex, FaceArrayIndex);
                    return nullptr;
                }
                FaceMaterialIndices[FaceArrayIndex] = MatIdx;
            }
        }

        for (int32 FaceIndex = 0; FaceIndex < NumFaces; ++FaceIndex)
        {
            if (FaceMaterialIndices[FaceIndex] == INDEX_NONE)
            {
                UE_LOG(LogTemp, Error, TEXT("UEFormat: skeletal face %d in LOD %d is not covered by any material"), FaceIndex, LodIndex);
                return nullptr;
            }
        }

        for (int32 FaceIndex = 0; FaceIndex < NumFaces; ++FaceIndex)
        {
            const int32 IndexBase = FaceIndex * 3;
            const int32 V0 = Data.Indices[IndexBase];
            const int32 V1 = Data.Indices[IndexBase + 1];
            const int32 V2 = Data.Indices[IndexBase + 2];
            if (!Data.Vertices.IsValidIndex(V0) || !Data.Vertices.IsValidIndex(V1) || !Data.Vertices.IsValidIndex(V2))
            {
                UE_LOG(LogTemp, Error, TEXT("UEFormat: invalid skeletal vertex index in LOD %d Face %d (%d, %d, %d; Vertices=%d)"),
                    LodIndex, FaceIndex, V0, V1, V2, Data.Vertices.Num());
                return nullptr;
            }

            SkeletalMeshImportData::FTriangle Face;
            FMemory::Memzero(Face);
            Face.MatIndex = static_cast<uint8>(FaceMaterialIndices[FaceIndex]);
            Face.SmoothingGroups = 255;

            for (int32 w = 0; w < 3; ++w)
            {
                    const int32 VIdx = Data.Indices[IndexBase + w];

                    FVector Normal(FVector::ZeroVector), Tangent(FVector::ZeroVector);
                    float   BinSign = 1.f;
                    if (ImportData.bHasNormals)
                    {
                        const FVector4& N4 = Data.Normals[VIdx];
                        BinSign = N4.X;
                        Normal  = FVector(N4.Y, N4.Z, N4.W).GetSafeNormal();
                    }
                    if (ImportData.bHasTangents)
                        Tangent = Data.Tangents[VIdx].GetSafeNormal();

                    Face.TangentX[w] = Tangent;
                    Face.TangentY[w] = FVector::CrossProduct(Normal, Tangent) * BinSign;
                    Face.TangentZ[w] = Normal;

                    SkeletalMeshImportData::FVertex Wedge;
                    Wedge.VertexIndex = static_cast<uint32>(VIdx);
                    Wedge.Color       = (Data.VertexColors.Num() > 0 && Data.VertexColors[0].Data.IsValidIndex(VIdx))
                                            ? Data.VertexColors[0].Data[VIdx]
                                            : FColor::White;
                    for (int32 ch = 0; ch < NumUVChannels; ++ch)
                    {
                        FVector2D UV = FVector2D::ZeroVector;
                        if (Data.TextureCoordinates.IsValidIndex(ch) && Data.TextureCoordinates[ch].IsValidIndex(VIdx))
                            UV = Data.TextureCoordinates[ch][VIdx];
                        Wedge.UVs[ch] = UV;
                    }

                    Face.WedgeIndex[w] = ImportData.Wedges.Add(Wedge);
            }
            ImportData.Faces.Add(Face);
        }

        TArray<TMap<int32, float>> WeightsByVertex;
        WeightsByVertex.SetNum(Data.Vertices.Num());
        for (const FWeightChunk& W : Data.Weights)
        {
            if (!Data.Vertices.IsValidIndex(W.WeightVertexIndex) ||
                !SkeletonData.Bones.IsValidIndex(W.WeightBoneIndex) ||
                !FMath::IsFinite(W.WeightAmount) || W.WeightAmount <= 0.0f)
            {
                UE_LOG(LogTemp, Warning, TEXT("UEFormat: skipped invalid influence in LOD %d (Vertex=%d Bone=%d Weight=%f)"),
                    LodIndex, W.WeightVertexIndex, W.WeightBoneIndex, W.WeightAmount);
                continue;
            }
            WeightsByVertex[W.WeightVertexIndex].FindOrAdd(W.WeightBoneIndex) += W.WeightAmount;
        }

        ImportData.Influences.Reserve(Data.Weights.Num() + Data.Vertices.Num());
        for (int32 VertexIndex = 0; VertexIndex < WeightsByVertex.Num(); ++VertexIndex)
        {
            TMap<int32, float>& VertexWeights = WeightsByVertex[VertexIndex];
            float TotalWeight = 0.0f;
            for (const TPair<int32, float>& Pair : VertexWeights)
                TotalWeight += Pair.Value;

            if (!FMath::IsFinite(TotalWeight) || TotalWeight <= SMALL_NUMBER)
            {
                SkeletalMeshImportData::FRawBoneInfluence RootInfluence;
                RootInfluence.BoneIndex = 0;
                RootInfluence.VertexIndex = VertexIndex;
                RootInfluence.Weight = 1.0f;
                ImportData.Influences.Add(RootInfluence);
                UE_LOG(LogTemp, Warning, TEXT("UEFormat: vertex %d in LOD %d had no valid weights and was assigned to the root bone"),
                    VertexIndex, LodIndex);
                continue;
            }

            for (const TPair<int32, float>& Pair : VertexWeights)
            {
                SkeletalMeshImportData::FRawBoneInfluence Inf;
                Inf.BoneIndex = Pair.Key;
                Inf.VertexIndex = VertexIndex;
                Inf.Weight = Pair.Value / TotalWeight;
                ImportData.Influences.Add(Inf);
            }
        }


        TArray<FVector>                                LODPoints;
        TArray<SkeletalMeshImportData::FMeshWedge>     LODWedges;
        TArray<SkeletalMeshImportData::FMeshFace>      LODFaces;
        TArray<SkeletalMeshImportData::FVertInfluence> LODInfluences;
        TArray<int32>                                  LODPointToRaw;

        ImportData.CopyLODImportData(LODPoints, LODWedges, LODFaces, LODInfluences, LODPointToRaw);

        if (LODFaces.Num() == 0 || LODWedges.Num() != LODFaces.Num() * 3)
        {
            UE_LOG(LogTemp, Error, TEXT("UEFormat: invalid skeletal topology before build in LOD %d (Faces=%d Wedges=%d)"),
                LodIndex, LODFaces.Num(), LODWedges.Num());
            return nullptr;
        }

        IMeshUtilities::MeshBuildOptions BuildOptions;
        BuildOptions.bComputeNormals  = !ImportData.bHasNormals;
        BuildOptions.bComputeTangents = !ImportData.bHasTangents;
        BuildOptions.bUseMikkTSpace   = true;

        bool bBuildOK = MeshUtils.BuildSkeletalMesh(
            LODModel,
            Name.ToString(),
            RefSkeleton,
            LODInfluences,
            LODWedges,
            LODFaces,
            LODPoints,
            LODPointToRaw,
            BuildOptions,
            nullptr,
            nullptr
        );

        if (!bBuildOK)
        {
            UE_LOG(LogTemp, Error, TEXT("UEFModelFactory: BuildSkeletalMesh failed for LOD %d"), LodIndex);
            return nullptr;
        }

        if (LodIndex == 0)
        {
            for (const FMorphTargetChunk& MorphChunk : Data.Morphs)
            {
                UMorphTarget* MorphTarget = NewObject<UMorphTarget>(SkeletalMesh, FName(MorphChunk.MorphName.c_str()));
                FMorphTargetLODModel MorphLOD;
                MorphLOD.NumBaseMeshVerts = Data.Vertices.Num();
                for (const FMorphTargetDataChunk& Delta : MorphChunk.MorphDeltas)
                {
                    if (!Data.Vertices.IsValidIndex(Delta.MorphVertexIndex))
                    {
                        UE_LOG(LogTemp, Warning, TEXT("UEFormat: skipped invalid morph vertex in '%s' (Vertex=%d Vertices=%d)"),
                            *FString(MorphChunk.MorphName.c_str()), Delta.MorphVertexIndex, Data.Vertices.Num());
                        continue;
                    }
                    FMorphTargetDelta MTD;
                    MTD.PositionDelta = FVector(Delta.MorphPosition.X, -Delta.MorphPosition.Y, Delta.MorphPosition.Z);
                    MTD.TangentZDelta = FVector::ZeroVector;
                    MTD.SourceIdx     = static_cast<uint32>(Delta.MorphVertexIndex);
                    MorphLOD.Vertices.Add(MTD);
                }
                MorphTarget->MorphLODModels.Add(MorphLOD);
                SkeletalMesh->GetMorphTargets().Add(MorphTarget);
                FAssetRegistryModule::AssetCreated(MorphTarget);
            }
        }
    }

    SkeletalMesh->ResetLODInfo();
    for (int32 i = 0; i < LODData.Num(); i++)
    {
        FSkeletalMeshLODInfo LODInfo;
        LODInfo.ScreenSize.Default = (i == 0) ? 1.f : (1.f / FMath::Pow(2.f, (float)i));
        SkeletalMesh->AddLODInfo(LODInfo);
    }

    Skeleton->MergeAllBonesToBoneTree(SkeletalMesh);
    Skeleton->SetPreviewMesh(SkeletalMesh);

    SkeletalMesh->InitMorphTargets();
    SkeletalMesh->CalculateInvRefMatrices();

    FBox ImportedBox(ForceInit);
    for (const FVector& Vertex : LODData[0].Vertices)
    {
        const FVector ScaledVertex = Vertex * ImportScale;
        if (!ScaledVertex.ContainsNaN())
            ImportedBox += ScaledVertex;
    }
    if (!ImportedBox.IsValid)
    {
        UE_LOG(LogTemp, Error, TEXT("UEFormat: failed to calculate skeletal mesh bounds"));
        return nullptr;
    }
    const float BoundsPadding = FMath::Max(1.0f, ImportedBox.GetExtent().GetMax() * 0.25f);
    SkeletalMesh->SetImportedBounds(FBoxSphereBounds(ImportedBox.ExpandBy(BoundsPadding)));
    UE_LOG(LogTemp, Display, TEXT("UEFormat: skeletal bounds Origin=%s Extent=%s Padding=%f"),
        *ImportedBox.GetCenter().ToString(), *ImportedBox.GetExtent().ToString(), BoundsPadding);

    SkeletalMesh->PostEditChange();

    Skeleton->PostEditChange();
    FAssetRegistryModule::AssetCreated(Skeleton);

    return SkeletalMesh;
}
