#include "Interface.vxx"

struct AutoBalance {
	field(R, VideoNode{});
	field(G, VideoNode{});
	field(B, VideoNode{});
	field(Radius, 0);

public:
	static constexpr auto Name = "AutoBalance";
	static constexpr auto Parameters = "clip:clip;radius:int:opt;";

public:
	AutoBalance(auto Arguments, auto Core) {
		auto Clip = static_cast<VideoNode>(Arguments["clip"]);
		if (Arguments["radius"].Exists())
			Radius = Arguments["radius"];
		if (!Clip.WithConstantFormat() || !Clip.WithConstantDimensions() || !Clip.IsSinglePrecision() || !Clip.IsRGB())
			throw RuntimeError{ "only RGBS input supported." };
		if (Radius < 0)
			throw RuntimeError{ "radius cannot be negative!" };

		R = Core["std"]["ShufflePlanes"]("clips", Clip, "planes", 0, "colorfamily", ColorFamilies::Gray);
		G = Core["std"]["ShufflePlanes"]("clips", Clip, "planes", 1, "colorfamily", ColorFamilies::Gray);
		B = Core["std"]["ShufflePlanes"]("clips", Clip, "planes", 2, "colorfamily", ColorFamilies::Gray);

		R = Core["std"]["PlaneStats"]("clipa", R);
		G = Core["std"]["PlaneStats"]("clipa", G);
		B = Core["std"]["PlaneStats"]("clipa", B);

		R.PaddingFunction = G.PaddingFunction = B.PaddingFunction = PaddingFunctions::Node::Repeat;
		R.RequestFunction = G.RequestFunction = B.RequestFunction = [Radius = Radius](auto Index) { return Range{ Index - Radius, Index + Radius + 1 }; };
	}

public:
	auto RegisterMetadata(auto Core) {
		auto Metadata = R.ExtractMetadata();
		Metadata.Format = Core.Query(VideoFormats::RGBS);
		return Metadata;
	}
	auto RequestReferenceFrames(auto Index, auto FrameContext) {
		R.RequestFrames(Index, FrameContext);
		G.RequestFrames(Index, FrameContext);
		B.RequestFrames(Index, FrameContext);
	}
	auto DrawFrame(auto Index, auto Core, auto FrameContext) {
		auto RFrames = R.FetchFrames<const float>(Index, FrameContext);
		auto GFrames = G.FetchFrames<const float>(Index, FrameContext);
		auto BFrames = B.FetchFrames<const float>(Index, FrameContext);

		auto ComponentR = Core.CopyFrame(RFrames[0]);
		auto ComponentG = Core.CopyFrame(GFrames[0]);
		auto ComponentB = Core.CopyFrame(BFrames[0]);

		auto EvaluateAccumulatedIntensities = [this](auto& ...References) {
			auto AverageIntensityOverTime = [this](auto& ReferenceFrames) {
				auto AccumulatedIntensity = 0.;
				auto CurrentSceneStartIndex = -Radius;
				auto CurrentSceneEndIndex = Radius + 1;
				for (auto t : Range{ -Radius })
					if (ReferenceFrames[t]["_SceneChangePrev"].Exists())
						if (auto SceneChangeTag = static_cast<bool>(ReferenceFrames[t]["_SceneChangePrev"]); SceneChangeTag) {
							CurrentSceneStartIndex = t;
							break;
						}
				for (auto t : Range{ CurrentSceneStartIndex, Radius + 1 })
					if (ReferenceFrames[t]["_SceneChangeNext"].Exists())
						if (auto SceneChangeTag = static_cast<bool>(ReferenceFrames[t]["_SceneChangeNext"]); SceneChangeTag) {
							CurrentSceneEndIndex = t + 1;
							break;
						}
				for (auto t : Range{ CurrentSceneStartIndex, CurrentSceneEndIndex })
					AccumulatedIntensity += static_cast<double>(ReferenceFrames[t]["PlaneStatsAverage"]);
				return AccumulatedIntensity / (CurrentSceneEndIndex - CurrentSceneStartIndex);
			};
			return std::array{ AverageIntensityOverTime(References)... };
		};

		constexpr auto Epsilon = 1e-20;
		auto [RIntensity, GIntensity, BIntensity] = EvaluateAccumulatedIntensities(RFrames, GFrames, BFrames);
		auto MaxIntensity = std::max({ RIntensity, GIntensity, BIntensity });
		auto RCorrection = MaxIntensity / std::max(RIntensity, Epsilon);
		auto GCorrection = MaxIntensity / std::max(GIntensity, Epsilon);
		auto BCorrection = MaxIntensity / std::max(BIntensity, Epsilon);
		auto NormalizationFactor = std::sqrt(RCorrection * RCorrection + GCorrection * GCorrection + BCorrection * BCorrection) / std::sqrt(3);
		RCorrection /= std::max(NormalizationFactor, Epsilon);
		GCorrection /= std::max(NormalizationFactor, Epsilon);
		BCorrection /= std::max(NormalizationFactor, Epsilon);

		for (auto y : Range{ ComponentR[0].Height })
			for (auto x : Range{ ComponentR[0].Width }) {
				ComponentR[0][y][x] *= RCorrection;
				ComponentG[0][y][x] *= GCorrection;
				ComponentB[0][y][x] *= BCorrection;
			}

		auto ProcessedFrame = Core.ShufflePlanes(std::array{ ComponentR, ComponentG, ComponentB }, 0, ColorFamilies::RGB);
		return ProcessedFrame.Leak();
	}
};

VS_EXTERNAL_API(auto) VapourSynthPluginInit(VSConfigPlugin configFunc, VSRegisterFunction registerFunc, VSPlugin* plugin) {
	VaporGlobals::Identifier = "com.adjust.wb";
	VaporGlobals::Namespace = "adjust";
	VaporGlobals::Description = "simple auto white balance filter";
	VaporInterface::RegisterPlugin(configFunc, plugin);
	VaporInterface::RegisterFilter<AutoBalance>(registerFunc, plugin);
}